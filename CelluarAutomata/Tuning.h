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
 * @note CAPACITY[] (energy) is indexed by CellType and checked the same way.
 * @note Energy is an integer; the numbers below are PLACEHOLDERS to be calibrated
 *       with headless runs once metabolism and food are in (steps E3/E4).
 */
namespace Tune {
    inline constexpr int   SEGMENT_HP = 10;     // hit points per segment
    inline constexpr float SIZE_BONUS = 4.0f;   // contest bonus = SIZE_BONUS / bodyLength
    inline constexpr float JITTER = 0.5f;   // random 0..JITTER added to each contest score

    // ---- energy (integer units; see World::totalEnergy and the energy ledger) ----
    inline constexpr int FOOD_MAX = 100;   // most food one tile can hold (also the full-scale for food color)
    inline constexpr int NEWBORN_ENERGY = 20;    // energy a freshly spawned worm starts with

    // Collision power contributed by each segment, indexed by CellType.
    //                              EMPTY HEAD MOVER STOR DEF OFF TAIL
    inline constexpr int ATTACK[] = { 0,   0,   0,    0,   0,  2,  0 };
    inline constexpr int DEFENSE[] = { 0,   0,   0,    0,   1,  0,  0 };
    // Energy capacity contributed by each segment, indexed by CellType.
    //                               EMPTY HEAD MOVER STOR DEF OFF TAIL
    inline constexpr int CAPACITY[] = { 0,  20,  10,   40,  10, 10, 10 };

    static_assert(sizeof(ATTACK) / sizeof(ATTACK[0]) == T_COUNT, "ATTACK needs one entry per CellType");
    static_assert(sizeof(DEFENSE) / sizeof(DEFENSE[0]) == T_COUNT, "DEFENSE needs one entry per CellType");
    static_assert(sizeof(CAPACITY) / sizeof(CAPACITY[0]) == T_COUNT, "CAPACITY needs one entry per CellType");
    static_assert(NEWBORN_ENERGY <= CAPACITY[T_HEAD], "a newborn is one head cell and its energy must fit in the capacity");
    static_assert(FOOD_MAX > 0 && FOOD_MAX <= 65535, "food is stored in a uint16_t");

    // ---- food production (World::environmentPhase) ----
    // FOOD_RATE is in milli-energy per tile per tick: 50 means 0.05 energy per tile per tick.
    // The world adds FOOD_RATE * (number of tiles) milli-energy to an accumulator every tick and
    // turns each whole FOOD_DROP * 1000 of it into one drop of FOOD_DROP energy on a random tile.
    // PLACEHOLDER values; they are calibrated once eating and metabolism exist.
    inline constexpr int FOOD_RATE = 50;
    inline constexpr int FOOD_DROP = 10;
    inline constexpr int FOOD_SEED_PERCENT = 30;   // World::seedFood: each tile gets a random 0..30% of FOOD_MAX

    static_assert(FOOD_RATE >= 0, "food rate cannot be negative");
    static_assert(FOOD_DROP > 0 && FOOD_DROP <= FOOD_MAX, "a drop must fit on a tile");
    static_assert(FOOD_SEED_PERCENT >= 0 && FOOD_SEED_PERCENT <= 100, "seed percent is 0..100");

    // ---- eating (World::eatPhase) ----
    // Each worm's genome has a biteSize: the most energy its head can take from the tile it
    // stands on in one tick. Genome::random draws it uniformly from BITE_MIN..BITE_MAX.
    // PLACEHOLDER values; they are calibrated once metabolism exists.
    inline constexpr int BITE_MIN = 10;
    inline constexpr int BITE_MAX = 30;

    static_assert(BITE_MIN >= 1 && BITE_MIN <= BITE_MAX && BITE_MAX <= 65535, "bite range must be ordered and fit in a uint16_t");
}