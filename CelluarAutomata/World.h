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
#include <string>
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
  *                  may mutate only scratch buffers and stats. APPLY and the phases
  *                  after it (EAT and ENVIRONMENT now; METABOLISM later) are the
  *                  only phases that change world state.
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
  *  I12 Validation. World::validate() is the executable form of this list. It checks
  *                  I1, I3-I6, I8 (no shared cells), I9 (facing), I10 and I13-I15,
  *                  plus the `deaths` bookkeeping. Whenever you add or change an
  *                  invariant here, change validate() in the same commit.
  *  I13 Energy pool. A living worm has 1 <= energy <= energyCapacity() (the sum of
  *                  Tune::CAPACITY over its segments); a dead worm has energy == 0.
  *                  A worm whose energy reaches 0 must die (starvation, from E3 on).
  *  I14 Food.       Every tile holds 0..Tune::FOOD_MAX food. Food belongs to the
  *                  tile: HexGrid::clearCell never touches it, and a tile with food
  *                  is still T_EMPTY for movement. The production carry-over
  *                  `foodAccum` stays in 0 .. Tune::FOOD_DROP*1000 - 1.
  *  I15 Ledger.     Energy is conserved: totalEnergy() (all living pools + all food)
  *                  == energyIn - energyOut, always. Energy may only be created by
  *                  adding to energyIn, destroyed by adding to energyOut, or moved
  *                  between a worm and tiles. Any other change is a bug.
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

    // Energy ledger. Every unit of energy that enters or leaves the world must be counted here,
    // so that  totalEnergy() == energyIn - energyOut  holds at all times.
    int64_t energyIn = 0;   // created: newborn endowments (later: food production)
    int64_t energyOut = 0;   // destroyed: heat (later), food that did not fit on a tile, ...

    // Food production carry-over, in milli-energy (see environmentPhase). Not energy yet: it
    // becomes energy, and is counted in energyIn, only when a drop actually lands on a tile.
    int64_t foodAccum = 0;

    // Statistic only, NOT part of the ledger: total energy ever moved from tiles into worms by
    // eating. (Eating moves energy; it neither creates nor destroys any.)
    int64_t energyEaten = 0;

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
    void clear() { grid.clear(); orgs.clear(); tick = 0; contests = deaths = 0; energyIn = energyOut = 0; foodAccum = 0; energyEaten = 0; }

    /** @brief Number of organisms with alive == true. O(orgs). HUD only. */
    int aliveCount() const {
        int n = 0;
        for (const auto& o : orgs) n += o.alive ? 1 : 0;
        return n;
    }

    // ---- helpers ----

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
     * @brief Hand a worm's remaining energy back to the tiles it occupies.
     * @param o The dying worm. Must still have its body (call BEFORE the body is cleared).
     * @post o.energy == 0. Energy is split evenly over the body's tiles, with any
     *       remainder going one unit at a time to the first segments (head first). A
     *       tile only accepts up to Tune::FOOD_MAX; whatever does not fit is
     *       dissipated and counted in energyOut, so the ledger stays exact.
     * Mutates: grid.food, o.energy, energyOut.
     * Called by: kill.
     * @note A worm that starved has 0 energy and returns nothing; one killed in a
     *       collision returns what it was holding.
     */
    void returnEnergyToTiles(Organism& o);

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
     * @brief Largest energy pool this worm can hold: the sum of Tune::CAPACITY over
     *        every segment's current type.
     * @return Non-negative integer. Pure function, O(body length).
     * Called by: validate (E2), and the eating/growth steps (E4/E6).
     * @note Capacity changes when the worm grows (segment types are derived from
     *       length, see I4). Keep Tune::CAPACITY[T_TAIL] no larger than the capacity
     *       of any pattern type, or clamp energy when a segment is added.
     */
    int energyCapacity(const Organism& o) const;

    /**
     * @brief All energy currently in the world: every living worm's pool plus all
     *        food on all tiles.
     * @return wormEnergy() + foodEnergy() as int64_t. O(orgs + w*h).
     * @note The energy ledger says this must equal energyIn - energyOut at all times.
     *       Dead worms hold 0 by definition (see returnEnergyToTiles) and are not summed.
     * Called by: validate (E2), tests.
     */
    int64_t totalEnergy() const;

    /** @brief Sum of the energy pools of all living worms. O(orgs). Called by: totalEnergy, the HUD, tests. */
    int64_t wormEnergy() const;

    /** @brief Sum of all food on all tiles. O(w*h). Called by: totalEnergy, the HUD, tests. */
    int64_t foodEnergy() const;

    /**
     * @brief ENVIRONMENT phase: put new food on the ground.
     *
     * Every tick FOOD_RATE * (w*h) milli-energy is added to `foodAccum`; each whole
     * FOOD_DROP*1000 of it becomes one drop of FOOD_DROP energy on a uniformly random
     * tile, topped up only to Tune::FOOD_MAX. Only the energy that actually lands is
     * counted in energyIn, so production slows down by itself as tiles fill, and the
     * ledger stays exact. The tile is drawn from `rng`, so this phase consumes random
     * numbers (see I11).
     * Mutates: grid.food, foodAccum, energyIn, rng.
     * Called by: step (last phase of a tick). Calls nothing else.
     * @note Food lands on any tile, occupied or not (I14: food belongs to the tile).
     *       Uniform placement is a placeholder: patches or gradients would go here.
     */
    void environmentPhase();

    /**
     * @brief Add a random amount of food to every tile, for a lived-in starting world.
     * @param percentOfMax Each tile receives a random 0..percentOfMax % of Tune::FOOD_MAX
     *                     (clamped to 0..100), topped up only to FOOD_MAX.
     * Mutates: grid.food, energyIn, rng. The energy created is counted in energyIn.
     * Called by: main (startup and after clear). Not called by step.
     */
    void seedFood(int percentOfMax);

    /**
     * @brief EAT phase: every living worm's head eats from the tile it stands on.
     *
     * For each living worm the amount taken is the smallest of: its genome's biteSize,
     * the food on the head's tile, and its free room (energyCapacity - energy). The
     * energy moves from the tile to the worm, so totalEnergy() does not change and
     * nothing is added to the ledger. Only the head eats. Because of follow-the-leader
     * movement the body segments stand on tiles the head just ate.
     * Two heads never share a tile (I8), so there are no eating conflicts and the
     * order of worms does not matter. Draws no random numbers.
     * Mutates: o.energy for living worms, grid.food, energyEaten.
     * Called by: step (after APPLY, before ENVIRONMENT). Calls: energyCapacity.
     * @note Dead worms do not eat (including worms killed earlier in the same tick).
     */
    void eatPhase();

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
     * 4. EAT:         living heads eat from their tile (eatPhase).
     * 5. ENVIRONMENT: new food appears on the ground (environmentPhase).
     *
     * Losers do not move this tick. Update order does not affect the outcome
     * (aside from RNG draw order, which is deterministic).
     * Mutates: grid, orgs, tick, contests, deaths, foodAccum, energyIn, energyEaten, and the scratch buffers.
     * Called by: main's fixed-timestep loop. Calls: decide, attackPower,
     * defensePower, applyMove, kill, retype, eatPhase, environmentPhase.
     * @warning Do not call spawn() from inside this function (I2).
     */
    void step();


    // ---- diagnostics ----
    /**
     * @brief Check the whole world against the invariants listed at the top of
     *        this file. This is the executable form of that list (see I12).
     * @return An empty string if every check passes. Otherwise a one-line
     *         description of the FIRST violation found, naming the organism,
     *         segment, cell and invariant involved.
     *
     * Checks, in order:
     *   A. grid arrays (type, orgId, hp, food) all hold exactly w*h elements
     *   B. per organism: dead worms hold no body/hp; living worms have a valid
     *      genome (I10), facing 0..5, body.size() == hp.size() >= 1 and at most
     *      genome.targetLength (I3); and for every segment: the cell is on the
     *      grid, no other segment owns it (I3/I8), it is adjacent to the
     *      previous segment (I3), and the grid agrees with the organism: type ==
     *      typeAt(i, len) and not empty (I4), orgId == id (I5), hp > 0, and the
     *      hp mirror matches (I5/I6); finally its energy is within
     *      1..energyCapacity (I13). A dead worm must hold no energy (I13)
     *   C. grid sweep: empty cells carry orgId 0 and hp 0, and every occupied
     *      cell belongs to a living worm's body (no orphan cells) (I5/I6); no
     *      tile holds more than Tune::FOOD_MAX food (I14)
     *   D. bookkeeping: `deaths` equals the number of dead organisms
     *   E. energy ledger: both counters are non-negative and totalEnergy()
     *      equals energyIn - energyOut (I15)
     *   F. food production carry-over `foodAccum` is within 0..FOOD_DROP*1000-1 (I14)
     *
     * Not checked (cannot be seen from the data): I2 pointer lifetimes, I7 phase
     * discipline, I9 for code paths that never store a direction, and I11 RNG
     * discipline.
     *
     * Cost: O(w*h + total body length), plus one w*h allocation per call. Cheap
     * enough to run every tick in debug builds on grids of this size.
     * Called by: main (debug builds, after every change to the world) and the
     * headless test project. Calls: HexGrid::neighbor, Genome::typeAt.
     * @note Corruption usually breaks several rules at once. Only the first one
     *       found is reported, in the order above and by organism index, so the
     *       message may describe a symptom rather than the root cause. Fix it
     *       and run again.
     * @note const and RNG-free: calling it never changes the simulation. A run
     *       with validation on is identical to one with it off.
     * @warning Keep this function in step with the invariants block. When an
     *          invariant is added or changed, change the matching check in the
     *          same commit; a stale validator is worse than none.
     */
    std::string validate() const;
};