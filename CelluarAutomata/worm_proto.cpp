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
#include "Organism.h"
#include "World.h"
#include "Render.h"

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

// --------------------------------------------------------------- Drawing --

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