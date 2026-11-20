#pragma once
/**
 * @file Render.h
 * @brief Drawing layer: hex geometry, view modes, and grid rendering.
 * Dependencies: raylib, HexGrid.h (read-only access). CellType.h and Tuning.h
 * are needed only by Render.cpp.
 *
 * Layering rule: this layer READS the grid and never changes the simulation.
 * It must not include World.h. Together with the driver it is the only code
 * that may include raylib.h.
 */
#include "raylib.h"
#include <cmath>       // std::round, std::fabs (HexLayout::pixelToCell)
#include "HexGrid.h"

inline constexpr float SQRT3 = 1.7320508f;

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

/**
 * @brief Which cell attribute cellColor() visualizes (cycled with TAB).
 * @warning VIEW_COUNT must stay last: the driver uses it to wrap the cycle and to
 *          size its VIEW_NAMES[] table. When adding a view, add its case in
 *          cellColor() (Render.cpp) and its name in VIEW_NAMES[] in main.
 */
enum ViewMode { VIEW_TYPE = 0, VIEW_ORGANISM, VIEW_HEALTH, VIEW_COUNT };

/**
 * @brief Color for one cell under the current view mode.
 * @param g    Grid to read (read-only).
 * @param i    Flat cell index.
 * @param view VIEW_TYPE, VIEW_ORGANISM, or VIEW_HEALTH.
 * Empty cells show their food in every view: black for none up to a fully
 * saturated green at Tune::FOOD_MAX. Organism view hashes orgId to a hue
 * (golden-angle spread); health view maps hp / SEGMENT_HP to red..green.
 * @note Reads grid.hp, which is only a mirror (I6). It is correct as of the
 *       last retype() and is for display only.
 * Called by: drawGrid.
 */
Color cellColor(const HexGrid& g, int i, int view);

/**
 * @brief Draw every cell as a filled hex (one DrawPoly per cell per frame).
 * @param outlines Draw cell outlines too (main passes zoom > 0.6).
 * @note O(w*h) draw calls. Fine at this size; this is the first thing to
 *       replace with a single mesh/texture if the grid gets large.
 * Must be called between BeginMode2D and EndMode2D.
 */
void drawGrid(const HexGrid& g, const HexLayout& L, bool outlines, int view);