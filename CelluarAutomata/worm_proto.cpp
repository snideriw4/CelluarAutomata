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

// Dependencies: CellType.h, Tuning.h, HexGrid.h, Genome, World, Render.
#include "CellType.h"
#include "Tuning.h"
#include "HexGrid.h"
#include "Genome.h"

/*
 * =============================================================================
 *  PROJECT INVARIANTS -- read before changing anything in World / Organism
 * =============================================================================
 *  I1  IDs.        Organism id = (index in World::orgs) + 1. Id 0 means "none".
 *                  World::orgs is append-only: dead organisms stay in place with
 *                  alive == false, so ids never change and are never reused.
 *  I2  Pointers.   World::spawn() push_back()s into orgs and may reallocate it.
 *                  Never hold an Organism reference/pointer across a spawn call.
 *  I3  Body.       For every alive organism: body.size() == hp.size() >= 1,
 *                  body[0] is the head, and body[i] is a hex neighbor of
 *                  body[i-1]. `body` holds grid POSITIONS and is shifted on
 *                  every move (push_front / pop_back). `hp` is indexed by
 *                  SEGMENT (0 = head) and is NOT shifted on a move; it only
 *                  grows, at the back, when the worm grows.
 *  I4  Types.      Cell types are never stored on the organism. They are derived
 *                  from the segment index via Genome::typeAt(i, len).
 *  I5  Grid sync.  After ANY change to body or hp, call World::retype().
 *                  retype() only writes cells that are IN the body, so any cell
 *                  the body vacates must be cleared by the caller with
 *                  HexGrid::clearCell().
 *  I6  Truth.      Occupancy: grid.type != T_EMPTY. Segment order and health:
 *                  Organism::body / Organism::hp. grid.hp is a render-only
 *                  mirror; never use it for simulation logic.
 *  I7  Phases.     DECIDE may mutate only the RNG and scratch buffers. RESOLVE
 *                  may mutate only scratch buffers and stats. APPLY is the only
 *                  phase that changes world state.
 *  I8  Targets.    decide() only targets cells that are EMPTY in the snapshot,
 *                  and resolve gives each cell at most one winner, so APPLY
 *                  moves can never overlap. Letting a head target an occupied
 *                  cell (for example a tail that is about to move) breaks this
 *                  guarantee and requires changes to resolve.
 *  I9  Directions. dir and rel are always 0..5 (0=E 1=NE 2=NW 3=W 4=SW 5=SE,
 *                  counter-clockwise). HexGrid::neighbor() returns -1 off the
 *                  edge, so always check. HexGrid::idx() does no bounds checks.
 *  I10 Genome.     period must be 1..4, growInterval >= 1, targetLength >= 1.
 *                  Zero causes modulo-by-zero; period > 4 reads past pattern[].
 *  I11 Randomness. All randomness comes from World::rng. Never put two
 *                  RNG-consuming calls in one argument list (C++ leaves the
 *                  evaluation order unspecified). std:: distributions are not
 *                  identical across standard libraries, so a seed replays
 *                  identically only for the same compiler + library build.
 * =============================================================================
 */

// ------------------------------------------------------------ Cell types --
static const Color TYPE_COLOR[T_COUNT] = {
    { 30,  30,  38, 255 },  // empty
    { 245, 245, 245, 255 },  // head
    { 240, 160,  50, 255 },  // mover
    { 240, 230,  80, 255 },  // storage
    { 70, 150, 230, 255 },  // defense
    { 230,  70,  70, 255 },  // offense
    { 170,  90, 220, 255 },  // tail
};

// -------------------------------------------------------------- Organism --
/**
 * @brief One worm: its genome plus its live body state.
 *
 * Members: `body` (grid positions, head first), `hp` (per-segment, same length
 * as body, indexed by segment not position), `facing` (absolute direction of
 * the last move, 0..5; random for a newborn), `alive`.
 *
 * @note See I1-I6 for the invariants tying this to the grid. Dead organisms
 *       keep their slot in World::orgs but have empty body/hp.
 */
struct Organism {
    Genome genome;
    std::deque<int> body;     // grid indices, head at front
    std::vector<int> hp;      // per-SEGMENT hit points, indexed like body (hp[0] = head)
    int facing = 0;
    bool alive = true;
};

// What a worm wants to do this tick. Produced in the DECIDE phase and never
// applied directly.
/**
 * @brief A proposed move produced by World::decide and consumed by World::step.
 *
 * `id` is the organism id (index + 1, see I1), `rel` the chosen direction
 * relative to facing at decide time, `target` the flat index of the cell it
 * wants (empty in the snapshot), and `score` its contest strength.
 *
 * @note An Intent is a request, not a result. Only the winner of each target
 *       cell is applied (see I8).
 */
struct Intent {
    uint32_t id;      // organism id (index + 1)
    int      rel;     // relative direction chosen
    int      target;  // grid index of the cell it wants to enter
    float    score;   // contest strength if someone else wants the same cell
};

// ----------------------------------------------------------------- World --
/**
 * @brief The whole simulation state plus the tick pipeline.
 *
 * Owns the grid, the organism list, the RNG, and per-tick scratch buffers.
 * Tick order (see step()): DECIDE -> RESOLVE -> APPLY. Rendering reads the
 * grid only and must never mutate the world.
 *
 * @note The scratch vectors (intents, winners, damage) are reused every tick
 *       and carry no meaning between ticks.
 */
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

    /**
     * @brief Remove every organism and reset the grid, tick and stats.
     * @note Does NOT reseed `rng`, so clearing and respawning is not a
     *       reproducible replay of an earlier run.
     * Called by: main (R key).
     */
    void clear() { grid.clear(); orgs.clear(); tick = 0; contests = deaths = 0; }

    /** @brief Number of organisms with alive == true. O(orgs). HUD only. */
    int aliveCount() const {
        int n = 0;
        for (const auto& o : orgs) n += o.alive ? 1 : 0;
        return n;
    }

    // ---- helpers ----

    /**
     * @brief Write an organism's body into the grid (type, orgId, hp mirror).
     * @param o  The organism to stamp.
     * @param id Its organism id (index + 1). Must match its slot in orgs (I1).
     * @pre  o.body.size() == o.hp.size() (I3), otherwise out-of-range read.
     * @post Every body cell has type = genome.typeAt(i, len), orgId = id, and
     *       hp = clamp(o.hp[i], 0, 255).
     * Called by: spawn, applyMove, step (after non-lethal damage).
     * @warning Does not clear cells the body no longer occupies (I5).
     */
    void retype(const Organism& o, uint32_t id) {
        int len = int(o.body.size());
        for (int i = 0; i < len; ++i) {
            grid.type[o.body[i]] = o.genome.typeAt(i, len);
            grid.orgId[o.body[i]] = id;
            grid.hp[o.body[i]] = uint8_t(std::clamp(o.hp[i], 0, 255));
        }
    }

    /**
     * @brief Total collision attack of a worm: the sum of Tune::ATTACK over
     *        every segment's current type.
     * @return Non-negative integer. Pure function, O(body length).
     * Called by: step (RESOLVE phase, on contest losers).
     * @note Whole-body for now; position along the body is ignored.
     */
    int attackPower(const Organism& o) const {
        int len = int(o.body.size()), sum = 0;
        for (int i = 0; i < len; ++i) sum += Tune::ATTACK[o.genome.typeAt(i, len)];
        return sum;
    }
    /**
     * @brief Total collision defense of a worm: the sum of Tune::DEFENSE over
     *        every segment's current type.
     * @return Non-negative integer. Pure function, O(body length).
     * Called by: step (RESOLVE phase, on the contest winner).
     * @note Whole-body for now; position along the body is ignored.
     */
    int defensePower(const Organism& o) const {
        int len = int(o.body.size()), sum = 0;
        for (int i = 0; i < len; ++i) sum += Tune::DEFENSE[o.genome.typeAt(i, len)];
        return sum;
    }

    /**
     * @brief Create a single-cell worm at `cellIdx`.
     * @param cellIdx Flat grid index. Must be in range [0, w*h); not checked.
     * @param g       Genome to give the newborn.
     * @return true if spawned; false if the cell was occupied.
     * @post The new organism is orgs.back() with id = orgs.size(), one segment
     *       (hp = Tune::SEGMENT_HP), random facing.
     * Called by: main (click), spawnRandom.
     * @warning May reallocate `orgs`, invalidating Organism references (I2).
     *          Never call during step().
     */
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

    /**
     * @brief Try to spawn up to `count` single-cell worms at random empty cells.
     * @param count Desired number of new worms.
     *
     * Makes at most count * 20 attempts, so it may spawn fewer than `count`
     * when the grid is crowded. The cell and the genome are drawn in separate
     * statements so the RNG draw order is defined (see I11).
     *
     * Called by: main (S key). Calls: Genome::random, spawn.
     * @warning May reallocate `orgs` (I2); never call during step().
     * @note For a given seed this produces a different layout than the old
     *       version did. That is expected, because the draw order is now fixed.
     */
    void spawnRandom(int count) {
        int total = grid.w * grid.h;
        for (int tries = 0, made = 0; made < count && tries < count * 20; ++tries) {
            int cell = int(rng() % total);   // draw 1: where
            Genome g = Genome::random(rng);  // draw 2: what
            if (spawn(cell, g)) ++made;
        }
    }

    /**
     * @brief Remove a worm from the world.
     * @param o The organism to kill. Must be alive with a valid body.
     * @post All its cells are cleared on the grid, body and hp are emptied,
     *       alive = false, and `deaths` is incremented.
     * Called by: step (when head HP reaches 0).
     * @note The slot stays in World::orgs (I1), so memory use grows with total
     *       organisms ever born. Dead organisms must be skipped by any code
     *       that touches body.front() (it is empty).
     */
    void kill(Organism& o) {
        for (int c : o.body) grid.clearCell(c);
        o.body.clear();
        o.hp.clear();
        o.alive = false;
        ++deaths;
    }

    // ---- phase 1: DECIDE (reads the grid, mutates nothing but the RNG) ----
    /**
     * @brief DECIDE phase: pick this worm's desired move from the current grid.
     * @param o   The worm (alive, non-empty body).
     * @param id  Its organism id (index + 1).
     * @param out Filled with the intent when the function returns true.
     * @return false if every neighbor is blocked or off-grid (no intent made).
     *
     * Chooses a relative direction randomly, weighted by genome.moveWeight[]
     * over neighbors that are empty in the snapshot. Score =
     * agility + Tune::SIZE_BONUS / bodyLength + Tune::JITTER * U(0,1).
     *
     * Reads: grid.type, grid.neighbor, o.body, o.facing, o.genome.
     * Writes: only `out` and the RNG (I7). RNG draws are consumed only when
     * it returns true.
     * Called by: step (DECIDE). Calls: HexGrid::neighbor.
     * @pre  o.body is non-empty (front() is used).
     * @post out.target is empty in the snapshot (I8).
     */
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
    /**
     * @brief APPLY phase: carry out one approved move.
     * @param o  The winning worm.
     * @param id Its organism id (index + 1).
     * @param in Its winning intent (target must be empty and adjacent to head).
     *
     * Pushes the target as the new head and updates facing. If the worm is
     * below its target length and tick % growInterval == 0 it grows: hp gets a
     * new segment at the BACK and the tail cell is kept. Otherwise the tail
     * cell is cleared and popped. Then retype() refreshes the grid.
     *
     * @note The asymmetry (body grows at the front, hp at the back) is
     *       deliberate: body holds positions, hp is indexed by segment (I3).
     * Reads: tick, o.genome. Writes: o.body, o.hp, o.facing, grid.
     * Called by: step (APPLY). Calls: HexGrid::clearCell, retype.
     * @warning Only valid for intents that won RESOLVE (see I8).
     */
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

    /**
     * @brief Advance the world by one tick.
     *
     * 1. DECIDE:  each alive worm proposes a move from the same snapshot.
     * 2. RESOLVE: intents are sorted by target cell; per cell the highest score
     *             wins. Each losing worm contributes max(0, its attack - the
     *             winner's defense) damage to the winner (accumulated in
     *             `damage`, indexed by organism id). `contests` counts cells
     *             claimed by two or more worms.
     * 3. APPLY:   winners move; then damage is subtracted from head HP
     *             (hp[0]); worms with hp[0] <= 0 are killed.
     *
     * Losers do not move this tick. Update order does not affect the outcome
     * (aside from RNG draw order, which is deterministic).
     * Mutates: grid, orgs, tick, contests, deaths and the scratch buffers.
     * Called by: main's fixed-timestep loop. Calls: decide, attackPower,
     * defensePower, applyMove, kill, retype.
     * @warning Do not call spawn() from inside this function (I2).
     */
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

/**
 * @brief Maps between grid cells and world-space (pre-camera) coordinates.
 *
 * Pointy-top hexes with circumradius R (center to corner), odd-r offset.
 * @warning center() and pixelToCell() are inverses and assume this exact
 *          orientation. Changing orientation means changing both together
 *          (and the DIRS table and the rotation passed to DrawPoly).
 */
struct HexLayout {
    float R = 12.0f;

    /** @brief World-space center of the hex at (col, row). */
    Vector2 center(int col, int row) const {
        return { SQRT3 * R * (col + 0.5f * (row & 1)), 1.5f * R * row };
    }

    /**
     * @brief Find which hex contains world-space point `p`.
     * @param p   Point in world space (already passed through the camera inverse).
     * @param col Output column (may be negative or past the grid).
     * @param row Output row (may be negative or past the grid).
     * Method: pixel -> fractional axial -> cube rounding -> odd-r offset.
     * @warning The outputs are NOT clamped. Always check HexGrid::inBounds()
     *          before using them as indices.
     * Called by: main (hover and click-to-spawn).
     */
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

/**
 * @brief Color for one cell under the current view mode.
 * @param g    Grid to read (read-only).
 * @param i    Flat cell index.
 * @param view VIEW_TYPE, VIEW_ORGANISM, or VIEW_HEALTH.
 * Empty cells are always the empty color. Organism view hashes orgId to a hue
 * (golden-angle spread); health view maps hp / SEGMENT_HP to red..green.
 * @note Reads grid.hp, which is only a mirror (I6). It is correct as of the
 *       last retype() and is for display only.
 * Called by: drawGrid.
 */
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

/**
 * @brief Draw every cell as a filled hex (one DrawPoly per cell per frame).
 * @param outlines Draw cell outlines too (main passes zoom > 0.6).
 * @note O(w*h) draw calls. Fine at this size; this is the first thing to
 *       replace with a single mesh/texture if the grid gets large.
 * Must be called between BeginMode2D and EndMode2D.
 */
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