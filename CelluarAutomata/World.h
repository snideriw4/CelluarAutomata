#pragma once
/**
 * @file World.h
 * @brief Simulation state and the tick pipeline (DECIDE -> RESOLVE -> APPLY).
 * Dependencies: HexGrid.h, Genome.h, Organism.h, Tuning.h. No raylib, by design:
 * the simulation must stay buildable and testable without any graphics code.
 * Used by: the driver (main) and the render layer (read-only).
 * Implementation: World.cpp (clear, aliveCount and the constructor stay inline).
 */
#include <cstdint>
#include <random>
#include <vector>
#include "Genome.h"
#include "HexGrid.h"
#include "Organism.h"

 /*
  * =============================================================================
  *  PROJECT INVARIANTS -- read before changing anything in World / Organism
  * =============================================================================
  *  I1  IDs.        Organism id = (index in World::orgs) + 1. Id 0 means "none".
  *                  World::orgs is append-only: dead organisms stay in place with
  *                  alive == false, so ids never change and are never reused.
  *  I2  Pointers.   World::spawn() push_back()s into orgs and may reallocate it.
  *                  Never hold an Organism reference/pointer across a spawn call.
  *  I3  Body.       For every alive organism: body.size() == hp.size() >= 1,
  *                  body[0] is the head, and body[i] is a hex neighbor of
  *                  body[i-1]. `body` holds grid POSITIONS and is shifted on
  *                  every move (push_front / pop_back). `hp` is indexed by
  *                  SEGMENT (0 = head) and is NOT shifted on a move; it only
  *                  grows, at the back, when the worm grows.
  *  I4  Types.      Cell types are never stored on the organism. They are derived
  *                  from the segment index via Genome::typeAt(i, len).
  *  I5  Grid sync.  After ANY change to body or hp, call World::retype().
  *                  retype() only writes cells that are IN the body, so any cell
  *                  the body vacates must be cleared by the caller with
  *                  HexGrid::clearCell().
  *  I6  Truth.      Occupancy: grid.type != T_EMPTY. Segment order and health:
  *                  Organism::body / Organism::hp. grid.hp is a render-only
  *                  mirror; never use it for simulation logic.
  *  I7  Phases.     DECIDE may mutate only the RNG and scratch buffers. RESOLVE
  *                  may mutate only scratch buffers and stats. APPLY is the only
  *                  phase that changes world state.
  *  I8  Targets.    decide() only targets cells that are EMPTY in the snapshot,
  *                  and resolve gives each cell at most one winner, so APPLY
  *                  moves can never overlap. Letting a head target an occupied
  *                  cell (for example a tail that is about to move) breaks this
  *                  guarantee and requires changes to resolve.
  *  I9  Directions. dir and rel are always 0..5 (0=E 1=NE 2=NW 3=W 4=SW 5=SE,
  *                  counter-clockwise). HexGrid::neighbor() returns -1 off the
  *                  edge, so always check. HexGrid::idx() does no bounds checks.
  *  I10 Genome.     period must be 1..4, growInterval >= 1, targetLength >= 1.
  *                  Zero causes modulo-by-zero; period > 4 reads past pattern[].
  *  I11 Randomness. All randomness comes from World::rng. Never put two
  *                  RNG-consuming calls in one argument list (C++ leaves the
  *                  evaluation order unspecified). std:: distributions are not
  *                  identical across standard libraries, so a seed replays
  *                  identically only for the same compiler + library build.
  * =============================================================================
  */

  /**
   * @brief The whole simulation state plus the tick pipeline.
   *
   * Owns the grid, the organism list, the RNG, and per-tick scratch buffers.
   * Tick order (see step()): DECIDE -> RESOLVE -> APPLY. Rendering reads the
   * grid only and must never mutate the world.
   *
   * @note The scratch vectors (intents, winners, damage) are reused every tick
   *       and carry no meaning between ticks.
   */
struct World {
    HexGrid grid;
    std::vector<Organism> orgs;
    std::mt19937 rng{ 12345 };
    uint64_t tick = 0;

    // stats for the HUD
    uint64_t contests = 0, deaths = 0;

    // scratch buffers reused every tick
    std::vector<Intent> intents, winners;
    std::vector<int> damage;

    World(int w, int h) : grid(w, h) {}

    /**
     * @brief Remove every organism and reset the grid, tick and stats.
     * @note Does NOT reseed `rng`, so clearing and respawning is not a
     *       reproducible replay of an earlier run.
     * Called by: main (R key).
     */
    void clear() { grid.clear(); orgs.clear(); tick = 0; contests = deaths = 0; }

    /** @brief Number of organisms with alive == true. O(orgs). HUD only. */
    int aliveCount() const {
        int n = 0;
        for (const auto& o : orgs) n += o.alive ? 1 : 0;
        return n;
    }

    // ---- helpers ----

    /**
     * @brief Write an organism's body into the grid (type, orgId, hp mirror).
     * @param o  The organism to stamp.
     * @param id Its organism id (index + 1). Must match its slot in orgs (I1).
     * @pre  o.body.size() == o.hp.size() (I3), otherwise out-of-range read.
     * @post Every body cell has type = genome.typeAt(i, len), orgId = id, and
     *       hp = clamp(o.hp[i], 0, 255).
     * Called by: spawn, applyMove, step (after non-lethal damage).
     * @warning Does not clear cells the body no longer occupies (I5).
     */
    void retype(const Organism& o, uint32_t id);

    /**
     * @brief Total collision attack of a worm: the sum of Tune::ATTACK over
     *        every segment's current type.
     * @return Non-negative integer. Pure function, O(body length).
     * Called by: step (RESOLVE phase, on contest losers).
     * @note Whole-body for now; position along the body is ignored.
     */
    int attackPower(const Organism& o) const;

    /**
     * @brief Total collision defense of a worm: the sum of Tune::DEFENSE over
     *        every segment's current type.
     * @return Non-negative integer. Pure function, O(body length).
     * Called by: step (RESOLVE phase, on the contest winner).
     * @note Whole-body for now; position along the body is ignored.
     */
    int defensePower(const Organism& o) const;

    /**
     * @brief Remove a worm from the world.
     * @param o The organism to kill. Must be alive with a valid body.
     * @post All its cells are cleared on the grid, body and hp are emptied,
     *       alive = false, and `deaths` is incremented.
     * Called by: step (when head HP reaches 0).
     * @note The slot stays in World::orgs (I1), so memory use grows with total
     *       organisms ever born. Dead organisms must be skipped by any code
     *       that touches body.front() (it is empty).
     */
    void kill(Organism& o);

    /**
     * @brief Create a single-cell worm at `cellIdx`.
     * @param cellIdx Flat grid index. Must be in range [0, w*h); not checked.
     * @param g       Genome to give the newborn.
     * @return true if spawned; false if the cell was occupied.
     * @post The new organism is orgs.back() with id = orgs.size(), one segment
     *       (hp = Tune::SEGMENT_HP), random facing.
     * Called by: main (click), spawnRandom.
     * @warning May reallocate `orgs`, invalidating Organism references (I2).
     *          Never call during step().
     */
    bool spawn(int cellIdx, const Genome& g);

    /**
     * @brief Try to spawn up to `count` single-cell worms at random empty cells.
     * @param count Desired number of new worms.
     *
     * Makes at most count * 20 attempts, so it may spawn fewer than `count`
     * when the grid is crowded. The cell and the genome are drawn in separate
     * statements so the RNG draw order is defined (see I11).
     *
     * Called by: main (S key). Calls: Genome::random, spawn.
     * @warning May reallocate `orgs` (I2); never call during step().
     * @note For a given seed this produces a different layout than the old
     *       version did. That is expected, because the draw order is now fixed.
     */
    void spawnRandom(int count);

    // ---- phase 1: DECIDE (reads the grid, mutates nothing but the RNG) ----
    /**
     * @brief DECIDE phase: pick this worm's desired move from the current grid.
     * @param o   The worm (alive, non-empty body).
     * @param id  Its organism id (index + 1).
     * @param out Filled with the intent when the function returns true.
     * @return false if every neighbor is blocked or off-grid (no intent made).
     *
     * Chooses a relative direction randomly, weighted by genome.moveWeight[]
     * over neighbors that are empty in the snapshot. Score =
     * agility + Tune::SIZE_BONUS / bodyLength + Tune::JITTER * U(0,1).
     *
     * Reads: grid.type, grid.neighbor, o.body, o.facing, o.genome.
     * Writes: only `out` and the RNG (I7). RNG draws are consumed only when
     * it returns true.
     * Called by: step (DECIDE). Calls: HexGrid::neighbor.
     * @pre  o.body is non-empty (front() is used).
     * @post out.target is empty in the snapshot (I8).
     */
    bool decide(const Organism& o, uint32_t id, Intent& out);

    // ---- phase 3 helper: perform an (already approved) move ----
    /**
     * @brief APPLY phase: carry out one approved move.
     * @param o  The winning worm.
     * @param id Its organism id (index + 1).
     * @param in Its winning intent (target must be empty and adjacent to head).
     *
     * Pushes the target as the new head and updates facing. If the worm is
     * below its target length and tick % growInterval == 0 it grows: hp gets a
     * new segment at the BACK and the tail cell is kept. Otherwise the tail
     * cell is cleared and popped. Then retype() refreshes the grid.
     *
     * @note The asymmetry (body grows at the front, hp at the back) is
     *       deliberate: body holds positions, hp is indexed by segment (I3).
     * Reads: tick, o.genome. Writes: o.body, o.hp, o.facing, grid.
     * Called by: step (APPLY). Calls: HexGrid::clearCell, retype.
     * @warning Only valid for intents that won RESOLVE (see I8).
     */
    void applyMove(Organism& o, uint32_t id, const Intent& in);

    /**
     * @brief Advance the world by one tick.
     *
     * 1. DECIDE:  each alive worm proposes a move from the same snapshot.
     * 2. RESOLVE: intents are sorted by target cell; per cell the highest score
     *             wins. Each losing worm contributes max(0, its attack - the
     *             winner's defense) damage to the winner (accumulated in
     *             `damage`, indexed by organism id). `contests` counts cells
     *             claimed by two or more worms.
     * 3. APPLY:   winners move; then damage is subtracted from head HP
     *             (hp[0]); worms with hp[0] <= 0 are killed.
     *
     * Losers do not move this tick. Update order does not affect the outcome
     * (aside from RNG draw order, which is deterministic).
     * Mutates: grid, orgs, tick, contests, deaths and the scratch buffers.
     * Called by: main's fixed-timestep loop. Calls: decide, attackPower,
     * defensePower, applyMove, kill, retype.
     * @warning Do not call spawn() from inside this function (I2).
     */
    void step();
};