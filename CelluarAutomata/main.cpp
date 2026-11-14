// Minimal multistate hex grid: pointy-top hexes, "odd-r" offset storage.
// Build with raylib (vcpkg: `vcpkg install raylib`, then link in Visual Studio).
//
// Controls: Left mouse = paint selected state, 1-8 = choose state,
//           Right mouse drag = pan, Mouse wheel = zoom.

#include "raylib.h"
#include <vector>
#include <cmath>
#include <cstdint>

// ---------------------------------------------------------------- Grid ----
//struct Cells {                      // SoA, all indexed via idx(c,r)
//    std::vector<uint8_t>  type;     // 0 = empty
//    std::vector<uint16_t> energy;
//    std::vector<uint32_t> orgId;    // 0 = none
//    std::vector<uint8_t>  signal;      // size = N * SIG_CH, read this tick
//    std::vector<uint8_t>  signalNext;  // written this tick, swapped after
//};
struct HexGrid {
    int w, h;
    std::vector<uint8_t> cells;            // flat: index = row * w + col

    HexGrid(int w_, int h_) : w(w_), h(h_), cells(size_t(w_)* h_, 0) {}

    bool inBounds(int c, int r) const { return c >= 0 && c < w && r >= 0 && r < h; }
    uint8_t& at(int c, int r) { return cells[size_t(r) * w + c]; }
    uint8_t  at(int c, int r) const { return cells[size_t(r) * w + c]; }
};

// ------------------------------------------------------------ Geometry ----
// Pointy-top hexes with circumradius R (center to corner).
constexpr float SQRT3 = 1.7320508f;

struct HexLayout {
    float R = 12.0f;

    // offset (odd-r) -> world-space center
    Vector2 center(int col, int row) const {
        return { SQRT3 * R * (col + 0.5f * (row & 1)), 1.5f * R * row };
    }

    // world-space point -> offset coords (via axial + cube rounding)
    void pixelToCell(Vector2 p, int& col, int& row) const {
        float q = (SQRT3 / 3.0f * p.x - 1.0f / 3.0f * p.y) / R;
        float r = (2.0f / 3.0f * p.y) / R;
        // cube round
        float x = q, z = r, y = -x - z;
        float rx = std::round(x), ry = std::round(y), rz = std::round(z);
        float dx = std::fabs(rx - x), dy = std::fabs(ry - y), dz = std::fabs(rz - z);
        if (dx > dy && dx > dz)      rx = -ry - rz;
        else if (dy > dz)            ry = -rx - rz;
        else                         rz = -rx - ry;
        int aq = int(rx), ar = int(rz);
        // axial -> odd-r offset
        row = ar;
        col = aq + (ar - (ar & 1)) / 2;
    }
};

// ---- Palette ---------------------------------------------------------
// State value -> color. Index 0 is the "empty" state.
static const Color PALETTE[] = {
    { 30,  30,  38, 255 },{ 230,  70,  70, 255 },{ 240, 160,  50, 255 },
    { 240, 230,  80, 255 },{ 90, 200,  90, 255 },{ 70, 190, 220, 255 },
    { 80, 100, 230, 255 },{ 170,  90, 220, 255 },{ 240, 240, 240, 255 },
};
constexpr int PALETTE_SIZE = sizeof(PALETTE) / sizeof(PALETTE[0]);

// ---- Drawing ---------------------------------------------------------
void drawGrid(const HexGrid& g, const HexLayout& L, bool outlines) {
    for (int r = 0; r < g.h; ++r) {
        for (int c = 0; c < g.w; ++c) {
            Vector2 ctr = L.center(c, r);
            Color col = PALETTE[g.at(c, r) % PALETTE_SIZE];
            // 6 sides, rotation 30 deg => pointy-top
            DrawPoly(ctr, 6, L.R, 30.0f, col);
            if (outlines) DrawPolyLines(ctr, 6, L.R, 30.0f, Fade(BLACK, 0.5f));
        }
    }
}

// ---- Main ------------------------------------------------------------
int main() {
    InitWindow(1280, 800, "Hex CA - grid prototype");
    SetTargetFPS(60);

    HexGrid grid(60, 40);
    HexLayout layout;
    int brush = 1;

    Camera2D cam{};
    cam.zoom = 1.0f;
    cam.offset = { 60, 60 };

    // Seed some variety so you can see the palette
    for (int r = 0; r < grid.h; ++r)
        for (int c = 0; c < grid.w; ++c)
            grid.at(c, r) = uint8_t((c / 7 + r / 5) % PALETTE_SIZE);

    while (!WindowShouldClose()) {
        // --- input ---
        for (int k = 0; k < 8; ++k)
            if (IsKeyPressed(KEY_ONE + k)) brush = k + 1;

        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT))
            cam.offset = { cam.offset.x + GetMouseDelta().x, cam.offset.y + GetMouseDelta().y };

        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            Vector2 before = GetScreenToWorld2D(GetMousePosition(), cam);
            cam.zoom *= (wheel > 0) ? 1.1f : 1.0f / 1.1f;
            Vector2 after = GetScreenToWorld2D(GetMousePosition(), cam);
            // keep the point under the cursor fixed
            cam.offset.x += (after.x - before.x) * cam.zoom;
            cam.offset.y += (after.y - before.y) * cam.zoom;
        }

        int hc = -1, hr = -1;
        Vector2 world = GetScreenToWorld2D(GetMousePosition(), cam);
        layout.pixelToCell(world, hc, hr);
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && grid.inBounds(hc, hr))
            grid.at(hc, hr) = uint8_t(brush);

        // --- draw ---
        BeginDrawing();
        ClearBackground(BLACK);
        BeginMode2D(cam);
        drawGrid(grid, layout, cam.zoom > 0.6f);
        if (grid.inBounds(hc, hr))
            DrawPolyLinesEx(layout.center(hc, hr), 6, layout.R, 30.0f, 2.0f / cam.zoom, WHITE);
        EndMode2D();

        DrawText(TextFormat("Brush: %d (keys 1-8)   Cell: %d,%d", brush, hc, hr), 10, 10, 20, RAYWHITE);
        DrawFPS(10, 36);
        EndDrawing();
    }
    CloseWindow();
    return 0;
}