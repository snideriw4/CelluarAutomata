/**
 * @file main.cpp
 * @brief Driver: window, input, camera, fixed-timestep loop, and HUD.
 *
 * MODULE MAP
 *   Simulation (no raylib; must stay buildable and testable without graphics)
 *     CellType.h        enum of cell kinds
 *     Tuning.h          balance constants (namespace Tune)
 *     HexGrid.h         flat structure-of-arrays grid + neighbor table
 *     Genome.h/.cpp     heritable body plan and behavior parameters
 *     Organism.h        per-worm state, Intent (a proposed move)
 *     World.h/.cpp      invariants I1-I11 and the tick pipeline
 *                       (DECIDE -> RESOLVE -> APPLY)
 *   Presentation (the only code allowed to include raylib.h)
 *     Render.h/.cpp     hex geometry, colors, view modes, drawing
 *     main.cpp          this file
 *
 * LAYERING RULES
 *   - Simulation files never include raylib.h or Render.h.
 *   - The render layer reads the grid; it never mutates the World.
 *      - World state changes only through World::step(), spawn(), spawnRandom()
 *        and clear(), called from the driver and never while drawing.
 */

/*
*   Controls
*        Left click   : spawn a single-cell baby (random genome) on an empty cell
*        S            : spawn 20 random babies (good for stress-testing contests)
*        Right drag   : pan          Mouse wheel : zoom
*        SPACE        : pause/resume Up/Down     : sim speed
*        TAB          : cycle view: cell type -> organism -> head/segment health
*        R            : clear world
*/

#include "raylib.h"
#include <algorithm>
#include <string>
#include <format>

#include "Genome.h"     // Genome::random (click-to-spawn)
#include "Render.h"     // HexLayout, ViewMode, drawGrid
#include "Tuning.h"     // Tune::FOOD_SEED_PERCENT
#include "World.h"      // World

/**
 * @brief Program entry: creates the window and world, then runs the main loop.
 *
 * Each frame: (1) read input; update the camera and the hover cell; (2) advance
 * the simulation with a fixed-timestep accumulator (ticksPerSec ticks per
 * second, at most 20 catch-up ticks per frame); (3) draw the grid and HUD.
 *
 * Reads/writes: World (spawn, spawnRandom, clear, step), Camera2D.
 * Calls: HexLayout::pixelToCell/center, drawGrid, raylib input/draw functions.
 * @note Simulation speed is independent of frame rate. Drawing never changes
 *       the world.
 * @warning VIEW_NAMES[] below must list one name per ViewMode, in enum order.
 */
int main() {
    InitWindow(1280, 800, "Cuppa Dirt Prototype");
    SetTargetFPS(60);

    World world(80, 60);
    HexLayout layout;

    Camera2D cam{};
    cam.zoom = 0.9f;
    cam.offset = { 40, 40 };

    world.seedFood(Tune::FOOD_SEED_PERCENT);

    bool paused = false;
    int view = VIEW_TYPE;
    float ticksPerSec = 10.0f;
    float accum = 0.0f;
    static const char* VIEW_NAMES[VIEW_COUNT] = { "type", "organism", "health" };


    // Debug-only self-check: validates the world after every change to it. On
    // the first violation it pauses the sim, remembers the message for the HUD
    // and logs it. Compiles to nothing in Release (NDEBUG) builds.
    std::string worldError;
    auto checkWorld = [&](const char* when) {
        #ifndef NDEBUG
            if (!worldError.empty()) return;              // keep the FIRST violation
            std::string err = world.validate();
            if (err.empty()) return;
            worldError = std::string("[") + when + ", tick " + std::to_string(world.tick) + "] " + err;
            paused = true;
            TraceLog(LOG_ERROR, "VALIDATE: %s", worldError.c_str());
        #else
            (void)when;
        #endif
     };

    while (!WindowShouldClose()) {
        // ---- input ----
        if (IsKeyPressed(KEY_SPACE)) paused = !paused;
        if (IsKeyPressed(KEY_TAB))   view = (view + 1) % VIEW_COUNT;
        if (IsKeyPressed(KEY_R)) { world.clear(); world.seedFood(Tune::FOOD_SEED_PERCENT); worldError.clear(); checkWorld("clear"); }
        if (IsKeyPressed(KEY_S)) { world.spawnRandom(20); checkWorld("spawnRandom"); }
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
        if (hoverOk && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            world.spawn(world.grid.idx(hc, hr), Genome::random(world.rng));
            checkWorld("spawn");
        }

        // ---- fixed-rate simulation ----
        if (!paused) {
            accum += GetFrameTime();
            float dt = 1.0f / ticksPerSec;
            int guard = 0;
            while (accum >= dt && guard++ < 20) {
                world.step();
                accum -= dt;
                checkWorld("step");
                if (!worldError.empty()) break;       // stop on the tick that broke it
            }
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

        const std::string hud = std::format("Tick {}  Alive {}/{}  Contests {}  Deaths {}  Energy {}  {:.1f} t/s {}  View: {}",
            world.tick, world.aliveCount(), world.orgs.size(), world.contests, world.deaths,
            world.totalEnergy(), ticksPerSec, paused ? "[PAUSED]" : "", VIEW_NAMES[view]);
        DrawText(hud.c_str(), 10, 10, 20, RAYWHITE);
        DrawText("Click: spawn | S: spawn 20 | Space: pause | Up/Down: speed | Tab: view | R: clear",
            10, 36, 16, GRAY);
        if (!worldError.empty()) DrawText(worldError.c_str(), 10, 62, 16, RED);
        EndDrawing();
    }
    CloseWindow();
    return 0;
}