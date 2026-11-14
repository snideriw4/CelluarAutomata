// Hex-grid worm prototype v2 (raylib): intent / resolve / apply ticks.
// Pointy-top hexes, odd-r offset storage.
//
// Controls
//   Left click   : spawn a single-cell baby (random genome) on an empty cell
//   S            : spawn 20 random babies (good for stress-testing contests)
//   Right drag   : pan          Mouse wheel : zoom
//   SPACE        : pause/resume Up/Down     : sim speed
//   TAB          : cycle view: cell type -> organism -> head/segment health
//   R            : clear world

#include "raylib.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <random>
#include <vector>

// ------------------------------------------------------------ Cell types --
enum CellType : uint8_t {
    T_EMPTY = 0, T_HEAD, T_MOVER, T_STORAGE, T_DEFENSE, T_OFFENSE, T_TAIL, T_COUNT
};

static const Color TYPE_COLOR[T_COUNT] = {
    { 30,  30,  38, 255 },  // empty
    { 245, 245, 245, 255 },  // head
    { 240, 160,  50, 255 },  // mover
    { 240, 230,  80, 255 },  // storage
    { 70, 150, 230, 255 },  // defense
    { 230,  70,  70, 255 },  // offense
    { 170,  90, 220, 255 },  // tail
};

// ---------------------------------------------------------------- Tuning --
// Everything you'll want to fiddle with while balancing contests.
namespace Tune {
    constexpr int   SEGMENT_HP = 10;     // hit points per segment
    constexpr float SIZE_BONUS = 4.0f;   // contest bonus = SIZE_BONUS / bodyLength
    constexpr float JITTER = 0.5f;   // random 0..JITTER added to each contest score

    // Collision power contributed by each segment, indexed by CellType.
    //                                    EMPTY HEAD MOVER STOR DEF OFF TAIL
    constexpr int ATTACK[T_COUNT] = { 0,   0,   0,    0,   0,  2,  0 };
    constexpr int DEFENSE[T_COUNT] = { 0,   0,   0,    0,   1,  0,  0 };
}

// ------------------------------------------------------------------ Grid --
// Directions, counter-clockwise from east: 0=E 1=NE 2=NW 3=W 4=SW 5=SE
static const int DIRS[2][6][2] = {
    { { +1, 0 },{ 0,-1 },{ -1,-1 },{ -1, 0 },{ -1,+1 },{ 0,+1 } },   // even rows
    { { +1, 0 },{ +1,-1 },{ 0,-1 },{ -1, 0 },{ 0,+1 },{ +1,+1 } },   // odd rows
};

struct HexGrid {
    int w, h;
    std::vector<uint8_t>  type;    // CellType
    std::vector<uint32_t> orgId;   // 0 = none, else index+1 into World::orgs
    std::vector<uint8_t>  hp;      // mirror of segment hp, for rendering

    HexGrid(int w_, int h_)
        : w(w_), h(h_), type(size_t(w_)* h_, T_EMPTY),
        orgId(size_t(w_)* h_, 0), hp(size_t(w_)* h_, 0) {
    }

    int  idx(int c, int r) const { return r * w + c; }
    bool inBounds(int c, int r) const { return c >= 0 && c < w && r >= 0 && r < h; }

    int neighbor(int index, int dir) const {
        int c = index % w, r = index / w;
        int nc = c + DIRS[r & 1][dir][0];
        int nr = r + DIRS[r & 1][dir][1];
        return inBounds(nc, nr) ? idx(nc, nr) : -1;
    }

    void clearCell(int i) { type[i] = T_EMPTY; orgId[i] = 0; hp[i] = 0; }

    void clear() {
        std::fill(type.begin(), type.end(), uint8_t(T_EMPTY));
        std::fill(orgId.begin(), orgId.end(), 0u);
        std::fill(hp.begin(), hp.end(), uint8_t(0));
    }
};

// ---------------------------------------------------------------- Genome --
struct Genome {
    uint8_t headType = T_HEAD;
    uint8_t tailType = T_TAIL;
    uint8_t period = 1;                 // length of repeating body pattern (1..4)
    uint8_t pattern[4] = { T_MOVER, T_MOVER, T_MOVER, T_MOVER };
    uint8_t targetLength = 10;
    uint8_t growInterval = 2;
    float   agility = 1.0f;             // contest strength when two heads want the same cell
    // Movement preference relative to facing: 0 straight, 1 60L, 2 120L,
    // 3 backward, 4 120R, 5 60R. Occupied/off-grid options are ignored.
    float moveWeight[6] = { 1, 1, 1, 1, 1, 1 };

    uint8_t typeAt(int i, int len) const {
        if (i == 0) return headType;
        if (i == len - 1) return tailType;
        return pattern[(i - 1) % period];
    }

    static Genome random(std::mt19937& rng) {
        std::uniform_real_distribution<float> u01(0.0f, 1.0f);
        Genome g;
        g.period = uint8_t(1 + rng() % 4);
        for (int i = 0; i < 4; ++i) g.pattern[i] = uint8_t(T_MOVER + rng() % 4);
        g.targetLength = uint8_t(5 + rng() % 21);
        g.growInterval = uint8_t(1 + rng() % 4);
        g.agility = 3.0f * u01(rng);
        for (int i = 0; i < 6; ++i) g.moveWeight[i] = 0.05f + 0.5f * u01(rng);
        static const int prefs[5] = { 0, 1, 2, 4, 5 };
        g.moveWeight[prefs[rng() % 5]] += 3.0f;
        return g;
    }
};

// -------------------------------------------------------------- Organism --
struct Organism {
    Genome genome;
    std::deque<int> body;     // grid indices, head at front
    std::vector<int> hp;      // per-SEGMENT hit points, indexed like body (hp[0] = head)
    int facing = 0;
    bool alive = true;
};

// What a worm wants to do this tick. Produced in the DECIDE phase and never
// applied directly.
struct Intent {
    uint32_t id;      // organism id (index + 1)
    int      rel;     // relative direction chosen
    int      target;  // grid index of the cell it wants to enter
    float    score;   // contest strength if someone else wants the same cell
};

// ----------------------------------------------------------------- World --
struct World {
    HexGrid grid;
    std::vector<Organism> orgs;
    std::mt19937 rng{ 12345 };
    uint64_t tick = 0;

    // stats for the HUD
    uint64_t contests = 0, deaths = 0;

    // scratch buffers reused every tick
    std::vector<Intent> intents, winners;
    std::vector<int> damage;

    World(int w, int h) : grid(w, h) {}

    void clear() { grid.clear(); orgs.clear(); tick = 0; contests = deaths = 0; }

    int aliveCount() const {
        int n = 0;
        for (const auto& o : orgs) n += o.alive ? 1 : 0;
        return n;
    }

    // ---- helpers ----
    void retype(const Organism& o, uint32_t id) {
        int len = int(o.body.size());
        for (int i = 0; i < len; ++i) {
            grid.type[o.body[i]] = o.genome.typeAt(i, len);
            grid.orgId[o.body[i]] = id;
            grid.hp[o.body[i]] = uint8_t(std::clamp(o.hp[i], 0, 255));
        }
    }

    int attackPower(const Organism& o) const {
        int len = int(o.body.size()), sum = 0;
        for (int i = 0; i < len; ++i) sum += Tune::ATTACK[o.genome.typeAt(i, len)];
        return sum;
    }
    int defensePower(const Organism& o) const {
        int len = int(o.body.size()), sum = 0;
        for (int i = 0; i < len; ++i) sum += Tune::DEFENSE[o.genome.typeAt(i, len)];
        return sum;
    }

    bool spawn(int cellIdx, const Genome& g) {
        if (grid.type[cellIdx] != T_EMPTY) return false;
        Organism o;
        o.genome = g;
        o.facing = int(rng() % 6);
        o.body.push_back(cellIdx);
        o.hp.assign(1, Tune::SEGMENT_HP);
        orgs.push_back(o);
        retype(orgs.back(), uint32_t(orgs.size()));
        return true;
    }

    void spawnRandom(int count) {
        int total = grid.w * grid.h;
        for (int tries = 0, made = 0; made < count && tries < count * 20; ++tries)
            if (spawn(int(rng() % total), Genome::random(rng))) ++made;
    }

    void kill(Organism& o) {
        for (int c : o.body) grid.clearCell(c);
        o.body.clear();
        o.hp.clear();
        o.alive = false;
        ++deaths;
    }

    // ---- phase 1: DECIDE (reads the grid, mutates nothing but the RNG) ----
    bool decide(const Organism& o, uint32_t id, Intent& out) {
        const Genome& g = o.genome;
        int head = o.body.front();

        double w[6];
        int target[6];
        double sum = 0.0;
        for (int rel = 0; rel < 6; ++rel) {
            int n = grid.neighbor(head, (o.facing + rel) % 6);
            target[rel] = n;
            bool free = (n >= 0 && grid.type[n] == T_EMPTY);
            w[rel] = free ? double(g.moveWeight[rel]) : 0.0;
            sum += w[rel];
        }
        if (sum <= 0.0) return false;                      // boxed in

        std::discrete_distribution<int> pick(w, w + 6);
        int rel = pick(rng);

        std::uniform_real_distribution<float> u01(0.0f, 1.0f);
        out.id = id;
        out.rel = rel;
        out.target = target[rel];
        out.score = g.agility
            + Tune::SIZE_BONUS / float(o.body.size())   // smaller worms get a bonus
            + Tune::JITTER * u01(rng);
        return true;
    }

    // ---- phase 3 helper: perform an (already approved) move ----
    void applyMove(Organism& o, uint32_t id, const Intent& in) {
        bool grow = int(o.body.size()) < o.genome.targetLength
            && (tick % o.genome.growInterval == 0);

        o.body.push_front(in.target);
        o.facing = (o.facing + in.rel) % 6;
        if (grow) {
            o.hp.push_back(Tune::SEGMENT_HP);       // new segment appended at the tail end
        }
        else {
            grid.clearCell(o.body.back());
            o.body.pop_back();
        }
        retype(o, id);
    }

    // ---- one simulation tick ----
    void step() {
        // 1. DECIDE: every living worm proposes a move from the same snapshot.
        intents.clear();
        for (size_t i = 0; i < orgs.size(); ++i) {
            Intent in;
            if (orgs[i].alive && decide(orgs[i], uint32_t(i + 1), in)) intents.push_back(in);
        }

        // 2. RESOLVE: group intents by target cell, pick one winner per cell.
        std::sort(intents.begin(), intents.end(), [](const Intent& a, const Intent& b) {
            return a.target != b.target ? a.target < b.target : a.id < b.id;
            });

        winners.clear();
        damage.assign(orgs.size() + 1, 0);
        for (size_t a = 0; a < intents.size();) {
            size_t b = a;
            while (b < intents.size() && intents[b].target == intents[a].target) ++b;

            size_t win = a;
            for (size_t k = a + 1; k < b; ++k)
                if (intents[k].score > intents[win].score) win = k;

            if (b - a > 1) {
                ++contests;
                // Every loser's collision may hurt the winner's head.
                int winnerDef = defensePower(orgs[intents[win].id - 1]);
                for (size_t k = a; k < b; ++k) {
                    if (k == win) continue;
                    int atk = attackPower(orgs[intents[k].id - 1]);
                    damage[intents[win].id] += std::max(0, atk - winnerDef);
                }
            }
            winners.push_back(intents[win]);
            a = b;
        }

        // 3. APPLY: winners move, then collision damage lands, then deaths.
        for (const Intent& in : winners) applyMove(orgs[in.id - 1], in.id, in);

        for (size_t i = 0; i < orgs.size(); ++i) {
            if (!orgs[i].alive || damage[i + 1] <= 0) continue;
            orgs[i].hp[0] -= damage[i + 1];
            if (orgs[i].hp[0] <= 0) kill(orgs[i]);      // head destroyed = organism dies (for now)
            else                    retype(orgs[i], uint32_t(i + 1));
        }
        ++tick;
    }
};

// -------------------------------------------------------------- Geometry --
constexpr float SQRT3 = 1.7320508f;

struct HexLayout {
    float R = 12.0f;

    Vector2 center(int col, int row) const {
        return { SQRT3 * R * (col + 0.5f * (row & 1)), 1.5f * R * row };
    }

    void pixelToCell(Vector2 p, int& col, int& row) const {
        float q = (SQRT3 / 3.0f * p.x - 1.0f / 3.0f * p.y) / R;
        float r = (2.0f / 3.0f * p.y) / R;
        float x = q, z = r, y = -x - z;
        float rx = std::round(x), ry = std::round(y), rz = std::round(z);
        float dx = std::fabs(rx - x), dy = std::fabs(ry - y), dz = std::fabs(rz - z);
        if (dx > dy && dx > dz) rx = -ry - rz;
        else if (dy > dz)       ry = -rx - rz;
        else                    rz = -rx - ry;
        int aq = int(rx), ar = int(rz);
        row = ar;
        col = aq + (ar - (ar & 1)) / 2;
    }
};

// --------------------------------------------------------------- Drawing --
enum ViewMode { VIEW_TYPE = 0, VIEW_ORGANISM, VIEW_HEALTH, VIEW_COUNT };

Color cellColor(const HexGrid& g, int i, int view) {
    uint8_t t = g.type[i];
    if (t == T_EMPTY) return TYPE_COLOR[T_EMPTY];
    switch (view) {
    case VIEW_ORGANISM: {
        float hue = std::fmod(float(g.orgId[i]) * 137.5f, 360.0f);
        return ColorFromHSV(hue, t == T_HEAD ? 0.25f : 0.65f, 0.95f);
    }
    case VIEW_HEALTH: {
        float frac = std::clamp(float(g.hp[i]) / float(Tune::SEGMENT_HP), 0.0f, 1.0f);
        return ColorFromHSV(120.0f * frac, 0.85f, 0.9f);      // red (hurt) -> green (healthy)
    }
    default:
        return TYPE_COLOR[t];
    }
}

void drawGrid(const HexGrid& g, const HexLayout& L, bool outlines, int view) {
    for (int r = 0; r < g.h; ++r) {
        for (int c = 0; c < g.w; ++c) {
            Vector2 ctr = L.center(c, r);
            DrawPoly(ctr, 6, L.R, 30.0f, cellColor(g, g.idx(c, r), view));
            if (outlines) DrawPolyLines(ctr, 6, L.R, 30.0f, Fade(BLACK, 0.5f));
        }
    }
}

// ------------------------------------------------------------------ Main --
int main() {
    InitWindow(1280, 800, "Hex worm prototype");
    SetTargetFPS(60);

    World world(60, 40);
    HexLayout layout;

    Camera2D cam{};
    cam.zoom = 0.9f;
    cam.offset = { 40, 40 };

    bool paused = false;
    int view = VIEW_TYPE;
    float ticksPerSec = 10.0f;
    float accum = 0.0f;
    static const char* VIEW_NAMES[VIEW_COUNT] = { "type", "organism", "health" };

    while (!WindowShouldClose()) {
        // ---- input ----
        if (IsKeyPressed(KEY_SPACE)) paused = !paused;
        if (IsKeyPressed(KEY_TAB))   view = (view + 1) % VIEW_COUNT;
        if (IsKeyPressed(KEY_R))     world.clear();
        if (IsKeyPressed(KEY_S))     world.spawnRandom(20);
        if (IsKeyPressed(KEY_UP))    ticksPerSec = std::min(240.0f, ticksPerSec * 1.5f);
        if (IsKeyPressed(KEY_DOWN))  ticksPerSec = std::max(0.5f, ticksPerSec / 1.5f);

        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
            Vector2 d = GetMouseDelta();
            cam.offset = { cam.offset.x + d.x, cam.offset.y + d.y };
        }
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            Vector2 before = GetScreenToWorld2D(GetMousePosition(), cam);
            cam.zoom *= (wheel > 0) ? 1.1f : 1.0f / 1.1f;
            Vector2 after = GetScreenToWorld2D(GetMousePosition(), cam);
            cam.offset.x += (after.x - before.x) * cam.zoom;
            cam.offset.y += (after.y - before.y) * cam.zoom;
        }

        int hc = -1, hr = -1;
        layout.pixelToCell(GetScreenToWorld2D(GetMousePosition(), cam), hc, hr);
        bool hoverOk = world.grid.inBounds(hc, hr);
        if (hoverOk && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            world.spawn(world.grid.idx(hc, hr), Genome::random(world.rng));

        // ---- fixed-rate simulation ----
        if (!paused) {
            accum += GetFrameTime();
            float dt = 1.0f / ticksPerSec;
            int guard = 0;
            while (accum >= dt && guard++ < 20) { world.step(); accum -= dt; }
            if (guard >= 20) accum = 0.0f;
        }

        // ---- draw ----
        BeginDrawing();
        ClearBackground(BLACK);
        BeginMode2D(cam);
        drawGrid(world.grid, layout, cam.zoom > 0.6f, view);
        if (hoverOk)
            DrawPolyLinesEx(layout.center(hc, hr), 6, layout.R, 30.0f, 2.0f / cam.zoom, WHITE);
        EndMode2D();

        DrawText(TextFormat("Tick %llu  Alive %d/%d  Contests %llu  Deaths %llu  %.1f t/s %s  View: %s",
            (unsigned long long)world.tick, world.aliveCount(), int(world.orgs.size()),
            (unsigned long long)world.contests, (unsigned long long)world.deaths,
            ticksPerSec, paused ? "[PAUSED]" : "", VIEW_NAMES[view]),
            10, 10, 20, RAYWHITE);
        DrawText("Click: spawn | S: spawn 20 | Space: pause | Up/Down: speed | Tab: view | R: clear",
            10, 36, 16, GRAY);
        EndDrawing();
    }
    CloseWindow();
    return 0;
}