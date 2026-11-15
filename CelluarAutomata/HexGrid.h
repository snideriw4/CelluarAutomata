#pragma once
/**
 * @file HexGrid.h
 * @brief Flat structure-of-arrays storage for the hex world, plus the
 *        neighbor-direction table.
 * Dependencies: CellType.h (T_EMPTY). No raylib, by design.
 * Used by: World (owns the grid), the render layer (read-only).
 */
#include <algorithm>   // std::fill
#include <cstdint>
#include <vector>
#include "CellType.h"

/**
 * @brief Neighbor offsets for odd-r offset coordinates, pointy-top hexes.
 *
 * Indexed as DIRS[row & 1][dir] = {dcol, drow}. Row parity matters because odd
 * rows are shifted half a cell right. Directions run counter-clockwise from
 * east: 0=E 1=NE 2=NW 3=W 4=SW 5=SE (see invariant I9).
 * @warning Genome::moveWeight[] is indexed by direction relative to facing and
 *          assumes this winding order. Reordering this table changes what
 *          "left" and "right" mean for every worm.
 * Used by: HexGrid::neighbor.
 */
inline constexpr int DIRS[2][6][2] = {
    { { +1, 0 },{ 0,-1 },{ -1,-1 },{ -1, 0 },{ -1,+1 },{ 0,+1 } },   // even rows
    { { +1, 0 },{ +1,-1 },{ 0,-1 },{ -1, 0 },{ 0,+1 },{ +1,+1 } },   // odd rows
};

/**
 * @brief Structure-of-arrays storage for the hex world.
 *
 * One entry per cell in each array; flat index = row * w + col. Pointy-top
 * hexes in "odd-r" offset layout (odd rows are shifted half a cell right).
 * Edges are hard walls (no wrapping).
 *
 * Arrays: type (CellType), orgId (see I1), hp (render mirror only, see I6), and
 *         food (energy lying on each tile, 0..Tune::FOOD_MAX). 
 *         Food belongs to the TILE: it is independent of what stands on the tile, 
 *         and a tile with food is still T_EMPTY for movement purposes.
 *
 * @note All arrays always hold exactly w*h elements. Never resize them after
 *       construction.
 * Used by: World (owns it), cellColor/drawGrid (read-only), main (hover/spawn).
 */
struct HexGrid {
    int w, h;
    std::vector<uint8_t>  type;    // CellType
    std::vector<uint32_t> orgId;   // 0 = none, else index+1 into World::orgs
    std::vector<uint8_t>  hp;      // mirror of segment hp, for rendering
    std::vector<uint16_t> food;    // energy on each tile (0..Tune::FOOD_MAX); NOT cleared by clearCell

    HexGrid(int w_, int h_)
        : w(w_), h(h_), type(size_t(w_)* h_, T_EMPTY),
        orgId(size_t(w_)* h_, 0), hp(size_t(w_)* h_, 0), food(size_t(w_)* h_, 0) {}

    /**
     * @brief Convert (column,row) to the flat array index.
     * @warning No bounds checking. Call inBounds(c, r) first for any coordinate
     *          that did not come from a known-valid index (e.g. mouse picking).
     */
    int  idx(int c, int r) const { return r * w + c; }
    /** @brief True if (c, r) lies inside the grid. Cheap; use before idx(). */
    bool inBounds(int c, int r) const { return c >= 0 && c < w && r >= 0 && r < h; }

    /**
     * @brief Index of the neighboring cell in direction `dir`.
     * @param index Flat index of the source cell (must be valid).
     * @param dir   0..5, counter-clockwise from east (0=E 1=NE 2=NW 3=W 4=SW 5=SE).
     * @return Flat index of the neighbor, or -1 if it falls off the grid.
     * Called by: World::decide.
     * @warning dir outside 0..5 indexes past DIRS[][] (undefined behavior).
     *          Callers compute it as (facing + rel) % 6.
     * @warning Always check for -1 before using the result as an index.
     * @note Row parity matters: the offsets come from DIRS[row & 1].
     */
    int neighbor(int index, int dir) const {
        int c = index % w, r = index / w;
        int nc = c + DIRS[r & 1][dir][0];
        int nr = r + DIRS[r & 1][dir][1];
        return inBounds(nc, nr) ? idx(nc, nr) : -1;
    }

    /**
     * @brief Reset one cell to empty (type, orgId and hp mirror).
     * @param i Flat cell index.
     * @note Touches only the grid, not any Organism. The caller is responsible
     *       for removing the cell from the owning organism's body (see I5).
     * @note Deliberately leaves `food` alone: food belongs to the tile, not to whatever
     *       was standing on it.
     * Called by: World::applyMove (vacated tail), World::kill.
     */
    void clearCell(int i) { type[i] = T_EMPTY; orgId[i] = 0; hp[i] = 0; }

    /**
     * @brief Reset every cell to empty and remove all food.
     * @note Grid only. World::clear() pairs this with orgs.clear(); calling it
     *       alone would leave organisms pointing at empty cells (breaks I3/I6).
     */
    void clear() {
        std::fill(type.begin(), type.end(), uint8_t(T_EMPTY));
        std::fill(orgId.begin(), orgId.end(), 0u);
        std::fill(hp.begin(), hp.end(), uint8_t(0));
        std::fill(food.begin(), food.end(), uint16_t(0));
    }
};