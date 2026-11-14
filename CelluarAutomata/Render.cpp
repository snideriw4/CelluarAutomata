/**
 * @file Render.cpp
 * @brief Implementation of the drawing layer. Contracts are documented with the
 *        declarations in Render.h.
 */
#include "Render.h"
#include <algorithm>   // std::clamp
#include <cmath>       // std::fmod
#include "CellType.h"  // T_EMPTY, T_HEAD, T_COUNT
#include "Tuning.h"    // Tune::SEGMENT_HP (full-scale for the health view)

static const Color TYPE_COLOR[] = {
    { 30,  30,  38, 255 },  // empty
    { 245, 245, 245, 255 },  // head
    { 240, 160,  50, 255 },  // mover
    { 240, 230,  80, 255 },  // storage
    { 70, 150, 230, 255 },  // defense
    { 230,  70,  70, 255 },  // offense
    { 170,  90, 220, 255 },  // tail
};
static_assert(sizeof(TYPE_COLOR) / sizeof(TYPE_COLOR[0]) == T_COUNT, "TYPE_COLOR needs one entry per CellType");

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