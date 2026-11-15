#pragma once
/**
 * @file Genome.h
 * @brief Heritable body plan and behavior parameters for a worm.
 * Dependencies: CellType.h (default member values use T_HEAD, T_MOVER, T_TAIL).
 * Used by: Organism.h (stored by value), World (typeAt in hot loops), main.
 * Implementation: Genome.cpp (random() only; everything else is inline here).
 */
#include <cstdint>
#include <random>      // std::mt19937 appears in random()'s signature
#include "CellType.h"

/**
 * @brief Heritable description of a worm's body plan and behavior.
 *
 * Body plan: head type, tail type, a repeating `pattern[0..period-1]` for the
 * middle segments, and a `targetLength` to grow to, adding one segment every
 * `growInterval` ticks. Behavior: `moveWeight[]` (preferred direction relative
 * to facing: 0 straight, 1 60L, 2 120L, 3 backward, 4 120R, 5 60R) and
 * `agility` (contest strength, see World::decide).
 *
 * @warning Must satisfy I10 (period 1..4, growInterval >= 1, targetLength >= 1).
 * @note A default-constructed Genome is valid.
 */
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

    /**
     * @brief Cell type of segment `i` in a body of `len` segments.
     * @param i   Segment index, 0 = head.
     * @param len Current body length (organism.body.size()).
     * @return A CellType value. Index 0 is always the head type, so a
     *         single-cell worm is a head; otherwise the last index is the tail
     *         type and everything between follows pattern[(i-1) % period].
     * @warning Types depend on `len`, so they change when the worm grows (the
     *          old tail index becomes a pattern segment). Call World::retype()
     *          after any length change (see I4/I5).
     * Called by: World::retype, attackPower, defensePower.
     */
    uint8_t typeAt(int i, int len) const {
        if (i == 0) return headType;
        if (i == len - 1) return tailType;
        return pattern[(i - 1) % period];
    }

    /**
     * @brief Build a random genome.
     * @param rng The world's RNG (advanced by this call; see I11).
     * @return Genome with period 1..4, targetLength 10..34, growInterval 1..4,
     *         agility 0..3, all four pattern slots random from MOVER..OFFENSE
     *         (only the first `period` are used), and moveWeights of 0.05..0.55
     *         plus a +3 bonus on one of {0,1,2,4,5} so each worm has a clear
     *         dominant tendency. Backward (3) is never the dominant direction.
     * Called by: main (click spawn), World::spawnRandom.
     */
    static Genome random(std::mt19937& rng);
};