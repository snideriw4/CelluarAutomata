#pragma once
/**
 * @file Organism.h
 * @brief Per-worm live state (Organism) and the move request type (Intent).
 * Dependencies: Genome.h (Organism holds a Genome by value).
 * Used by: World. Plain data only; all behavior lives in World (World.h/.cpp).
 * @note The invariants tying Organism to the grid (I1-I6) are documented at the top of World.h
 */
#include <cstdint>
#include <deque>
#include <vector>
#include "Genome.h"

 /**
  * @brief One worm: its genome plus its live body state.
  *
  * Members: `body` (grid positions, head first), `hp` (per-segment, same length
  * as body, indexed by segment not position), `facing` (absolute direction of
  * the last move, 0..5; random for a newborn), `alive`, and `energy`.
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
    int energy = 0;           // the worm's energy pool (integer units); 0 for a dead worm.
                              // Bounded above by World::energyCapacity(); see the energy ledger.
};

// What a worm wants to do this tick. Produced in the DECIDE phase and never applied directly.
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