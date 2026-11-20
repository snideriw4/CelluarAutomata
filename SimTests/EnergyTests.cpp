/**
 * @file EnergyTests.cpp
 * @brief Tests of the energy system itself (not of validate(), which has its own
 *        file): newborn endowment, the conservation ledger, and what happens to a
 *        worm's energy when it dies. Later steps add starvation and eating here.
 *
 * These checks are written independently of World::validate() on purpose: if the
 * validator had a blind spot, tests that relied on it would share that blind spot.
 *
 * Dependencies: TestHarness.h, Tuning.h, World.h. Called by: main.cpp.
 */
#include "TestHarness.h"
#include "Tuning.h"
#include <algorithm>
#include <vector>

namespace th {

    /** @brief Index of the first living worm with at least `minLen` segments, or orgs.size(). */
    static size_t findWorm(const World& w, size_t minLen) {
        for (size_t i = 0; i < w.orgs.size(); ++i)
            if (w.orgs[i].alive && w.orgs[i].body.size() >= minLen) return i;
        return w.orgs.size();
    }

    /** @brief A small world that has run long enough to contain multi-segment worms. */
    static World makeBase() {
        World w(30, 20);
        w.rng.seed(7);
        w.spawnRandom(10);
        for (int t = 0; t < 60; ++t) w.step();
        return w;
    }

    /** Spawning endows exactly NEWBORN_ENERGY and counts it; a failed spawn changes nothing. */
    static void testNewbornEndowment(Stats& s) {
        World w(10, 10);
        check(s, w.spawn(0, Genome()), "spawn on an empty tile succeeds");
        check(s, w.orgs[0].energy == Tune::NEWBORN_ENERGY, "newborn holds NEWBORN_ENERGY");
        check(s, w.energyIn == Tune::NEWBORN_ENERGY && w.energyOut == 0, "endowment is counted as created energy");
        check(s, w.totalEnergy() == Tune::NEWBORN_ENERGY, "total energy equals the endowment");

        const int64_t inBefore = w.energyIn;
        check(s, !w.spawn(0, Genome()), "spawn on an occupied tile fails");
        check(s, w.energyIn == inBefore && w.orgs.size() == 1, "a failed spawn creates no energy and no organism");
    }

    /** The conservation identity and pool bounds hold at every tick, across many seeds. */
    static void testLedgerAndBounds(Stats& s) {
        long checks = 0;
        bool good = true;
        for (int seed = 1; seed <= 25 && good; ++seed) {
            const bool small = (seed % 2) != 0;
            World w(small ? 30 : 60, small ? 20 : 40);
            w.rng.seed(uint32_t(seed));
            w.spawnRandom(seed % 3 == 0 ? 120 : 40);

            auto verify = [&](const char* when, int tick) {
                ++checks;
                std::string problem;
                if (w.totalEnergy() != w.energyIn - w.energyOut)
                    problem = "ledger: total " + std::to_string(w.totalEnergy()) + " != in " +
                    std::to_string(w.energyIn) + " - out " + std::to_string(w.energyOut);
                for (size_t i = 0; problem.empty() && i < w.orgs.size(); ++i) {
                    const Organism& o = w.orgs[i];
                    if (o.alive && (o.energy < 1 || o.energy > w.energyCapacity(o)))
                        problem = "organism " + std::to_string(i + 1) + " energy " + std::to_string(o.energy) + " out of bounds";
                    if (!o.alive && o.energy != 0)
                        problem = "dead organism " + std::to_string(i + 1) + " holds energy";
                }
                if (problem.empty()) return;
                good = false;
                check(s, false, "energy ledger and pool bounds", "seed " + std::to_string(seed) + ", " + when +
                    ", tick " + std::to_string(tick) + ": " + problem);
                };

            verify("after spawn", 0);
            for (int t = 0; t < 800 && good; ++t) {
                w.step();
                verify("after step", t);
                if (t % 100 == 50) { w.spawnRandom(5); verify("after respawn", t); }
                if (t == 400 && seed % 5 == 0) { w.clear(); verify("after clear", t); w.spawnRandom(30); }
            }
        }
        check(s, good, "ledger identity and pool bounds hold for " + std::to_string(checks) + " checkpoints");
    }

    /** A dying worm's energy is split evenly over its tiles; the remainder goes head-first. */
    static void testDeathReturnsEnergyEvenly(Stats& s) {
        World w = makeBase();
        const size_t K = findWorm(w, 3);
        check(s, K != w.orgs.size(), "setup: found a worm with >= 3 segments");
        if (K == w.orgs.size()) return;

        Organism& o = w.orgs[K];
        const std::vector<int> cells(o.body.begin(), o.body.end());
        const int n = int(cells.size());

        // Start from empty tiles, and keep the ledger honest about every manual edit.
        for (int c : cells) { w.energyOut += w.grid.food[size_t(c)]; w.grid.food[size_t(c)] = 0; }
        const int pool = 3 * n + 2;                       // share 3 per tile, remainder 2
        w.energyIn += pool - o.energy;
        o.energy = pool;
        const int64_t totalBefore = w.totalEnergy();
        const int64_t outBefore = w.energyOut;

        w.kill(o);

        check(s, w.grid.food[size_t(cells[0])] == 4, "head tile gets share + 1");
        check(s, w.grid.food[size_t(cells[1])] == 4, "second tile gets share + 1");
        bool rest = true;
        for (int i = 2; i < n; ++i) rest = rest && (w.grid.food[size_t(cells[size_t(i)])] == 3);
        check(s, rest, "remaining tiles get the plain share");
        check(s, w.totalEnergy() == totalBefore, "a death with room on the tiles conserves total energy");
        check(s, w.energyOut == outBefore, "nothing is dissipated when everything fits");
        check(s, !w.orgs[K].alive && w.orgs[K].energy == 0, "the dead worm holds 0 energy");
        check(s, w.totalEnergy() == w.energyIn - w.energyOut, "ledger exact after the death");
    }

    /** Tiles accept at most FOOD_MAX; the excess is dissipated and counted, never lost silently. */
    static void testOverflowIsDissipatedAndCounted(Stats& s) {
        World w = makeBase();
        const size_t K = findWorm(w, 3);
        check(s, K != w.orgs.size(), "setup: found a worm with >= 3 segments");
        if (K == w.orgs.size()) return;

        Organism& o = w.orgs[K];
        const std::vector<int> cells(o.body.begin(), o.body.end());
        for (int c : cells) {                              // fill the tiles; account for the energy we add
            w.energyIn += Tune::FOOD_MAX - w.grid.food[size_t(c)];
            w.grid.food[size_t(c)] = uint16_t(Tune::FOOD_MAX);
        }
        const int held = o.energy;
        const int64_t outBefore = w.energyOut;

        w.kill(o);

        bool allFull = true;
        for (int c : cells) allFull = allFull && (w.grid.food[size_t(c)] == Tune::FOOD_MAX);
        check(s, allFull, "full tiles stay at FOOD_MAX");
        check(s, w.energyOut - outBefore == held, "the worm's whole energy (" + std::to_string(held) + ") is counted as dissipated");
        check(s, w.totalEnergy() == w.energyIn - w.energyOut, "ledger exact after an overflowing death");
    }

    /** Food belongs to the tile: clearCell leaves it, clear() removes everything. */
    static void testFoodBelongsToTheTile(Stats& s) {
        World w(30, 20);
        w.spawnRandom(10);
        w.grid.food[5] = 7;
        w.grid.type[5] = T_MOVER;
        w.grid.clearCell(5);
        check(s, w.grid.food[5] == 7, "HexGrid::clearCell leaves the tile's food alone");

        w.clear();
        bool allZero = true;
        for (uint16_t f : w.grid.food) allZero = allZero && (f == 0);
        check(s, allZero && w.orgs.empty(), "World::clear removes all food and all worms");
        check(s, w.energyIn == 0 && w.energyOut == 0 && w.totalEnergy() == 0, "World::clear resets the ledger");
    }

    /** With no worms, production creates exactly the configured amount, all of it on the ground. */
    static void testFoodProductionRate(Stats& s) {
        World w(80, 60);
        const int cells = 80 * 60;
        const int ticks = 100;                              // short enough that no tile fills up
        for (int t = 0; t < ticks; ++t) w.step();

        const int64_t unit = int64_t(Tune::FOOD_DROP) * 1000;
        const int64_t milli = int64_t(Tune::FOOD_RATE) * cells * ticks;
        const int64_t expected = (milli / unit) * Tune::FOOD_DROP;
        check(s, w.energyIn == expected, "production over " + std::to_string(ticks) + " ticks creates the configured amount",
            "expected " + std::to_string(expected) + ", got " + std::to_string(w.energyIn));
        check(s, w.foodAccum == milli % unit, "the remainder is carried in foodAccum");
        check(s, w.totalEnergy() == w.energyIn && w.energyOut == 0, "all of it is on the ground and nothing was destroyed");
    }

    /** Production never overfills a tile, and stops creating energy once everything is full. */
    static void testFoodSaturates(Stats& s) {
        World w(20, 15);
        const int cells = 20 * 15;
        const int64_t capacity = int64_t(cells) * Tune::FOOD_MAX;
        const int ticks = 8 * Tune::FOOD_MAX * 1000 / std::max(1, Tune::FOOD_RATE);     // many times the fill time
        for (int t = 0; t < ticks; ++t) w.step();

        bool capped = true;
        for (uint16_t f : w.grid.food) capped = capped && (f <= Tune::FOOD_MAX);
        check(s, capped, "no tile ever exceeds FOOD_MAX");
        check(s, w.energyIn <= capacity && w.totalEnergy() == w.energyIn, "created energy never exceeds the ground's capacity");
        check(s, w.energyIn * 100 >= capacity * 99, "after a long time the ground is (almost) completely full");

        const int64_t before = w.energyIn;
        for (int t = 0; t < 500; ++t) w.step();
        check(s, w.energyIn - before <= capacity - before, "once full, production stops creating energy");
    }

    /** seedFood adds a bounded random amount, never overfills, and keeps the ledger exact. */
    static void testSeedFood(Stats& s) {
        World w(30, 20);
        w.seedFood(30);
        bool bounded = true;
        int64_t sum = 0;
        for (uint16_t f : w.grid.food) { bounded = bounded && (f <= Tune::FOOD_MAX * 30 / 100); sum += f; }
        check(s, bounded, "seedFood(30) puts at most 30% of FOOD_MAX on any tile");
        check(s, sum > 0 && w.energyIn == sum && w.totalEnergy() == sum, "seeded food is counted as created energy");

        World zero(30, 20);
        zero.seedFood(0);
        check(s, zero.totalEnergy() == 0 && zero.energyIn == 0, "seedFood(0) adds nothing");

        World full(10, 10);
        full.seedFood(100);
        full.seedFood(100);
        bool capped = true;
        for (uint16_t f : full.grid.food) capped = capped && (f <= Tune::FOOD_MAX);
        check(s, capped && full.totalEnergy() == full.energyIn, "seeding twice never overfills a tile and the ledger stays exact");

        World c(10, 10);
        for (int t = 0; t < 5; ++t) c.step();
        c.clear();
        check(s, c.foodAccum == 0 && c.totalEnergy() == 0, "World::clear resets the production carry-over");
    }

    void runEnergyTests(Stats& s) {
        std::printf("[energy]\n");
        testFoodProductionRate(s);
        testFoodSaturates(s);
        testSeedFood(s);
        testNewbornEndowment(s);
        testLedgerAndBounds(s);
        testDeathReturnsEnergyEvenly(s);
        testOverflowIsDissipatedAndCounted(s);
        testFoodBelongsToTheTile(s);
    }

} // namespace th