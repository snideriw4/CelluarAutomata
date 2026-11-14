#pragma once
/**
 * @file Tuning.h
 * @brief Central balance constants for the simulation.
 * Dependencies: CellType.h (for T_COUNT).
 * Used by: World (spawn, decide, attack/defense power, step) and the render
 * layer (SEGMENT_HP scales the health view).
 */
#include "CellType.h"

/**
 * @brief Central tuning constants. Change balance numbers here, not in logic.
 *
 * @note SEGMENT_HP is also the full-scale value for the health view (cellColor).
 * @note ATTACK[] and DEFENSE[] are indexed by CellType, one entry per type in
 *       enum order. They have no explicit size and are checked by static_assert,
 *       so adding a CellType without extending both tables is a compile error.
 *       TYPE_COLOR[] in Render.cpp is checked the same way.
 */
namespace Tune {
    inline constexpr int   SEGMENT_HP = 10;     // hit points per segment
    inline constexpr float SIZE_BONUS = 4.0f;   // contest bonus = SIZE_BONUS / bodyLength
    inline constexpr float JITTER = 0.5f;   // random 0..JITTER added to each contest score

    // Collision power contributed by each segment, indexed by CellType.
    //                                    EMPTY HEAD MOVER STOR DEF OFF TAIL
    inline constexpr int ATTACK[] = { 0,   0,   0,    0,   0,  2,  0 };
    inline constexpr int DEFENSE[] = { 0,   0,   0,    0,   1,  0,  0 };

    static_assert(sizeof(ATTACK) / sizeof(ATTACK[0]) == T_COUNT, "ATTACK needs one entry per CellType");
    static_assert(sizeof(DEFENSE) / sizeof(DEFENSE[0]) == T_COUNT, "DEFENSE needs one entry per CellType");
}