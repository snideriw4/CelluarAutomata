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

    /** @brief Set a tile's food by hand, keeping the energy ledger exact. */
    static void setFood(World& w, size_t tile, int amount) {
        const int before = w.grid.food[tile];
        if (amount >= before) w.energyIn += amount - before; else w.energyOut += before - amount;
        w.grid.food[tile] = uint16_t(amount);
    }

    /** @brief Set a worm's energy by hand, keeping the energy ledger exact. */
    static void setEnergy(World& w, Organism& o, int amount) {
        if (amount >= o.energy) w.energyIn += amount - o.energy; else w.energyOut += o.energy - amount;
        o.energy = amount;
    }

    /**
     * @brief One single-cell worm (the given bite) on tile 55 of a 10x10 world, with `food` on
     *        its tile and exactly `room` free energy room. The ledger stays exact.
     * @note room must be 0 .. capacity-1 so the worm keeps at least 1 energy.
     */
    static World makeEater(int bite, int room, int food) {
        World w(10, 10);
        Genome g;
        g.biteSize = uint16_t(bite);
        w.spawn(55, g);
        setEnergy(w, w.orgs[0], w.energyCapacity(w.orgs[0]) - room);
        setFood(w, 55, food);
        return w;
    }

    /** A bite is the smallest of: biteSize, the food on the tile, and the worm's free room. */
    static void testEatingMovesEnergyFromTileToWorm(Stats& s) {
        struct Case { const char* name; int bite, room, food, taken; };
        const Case cases[] = {
            { "limited by biteSize",        5, 8, 50, 5 },
            { "limited by the tile's food", 5, 8,  3, 3 },
            { "limited by free room",       5, 2, 50, 2 },
            { "a full worm eats nothing",   5, 0, 50, 0 },
            { "a worm with bite 0",         0, 8, 50, 0 },
            { "an empty tile",              5, 8,  0, 0 },
        };
        for (const Case& c : cases) {
            World w = makeEater(c.bite, c.room, c.food);
            const int energyBefore = w.orgs[0].energy;
            const int64_t total = w.totalEnergy();
            w.eatPhase();
            check(s, w.orgs[0].energy == energyBefore + c.taken && w.grid.food[55] == c.food - c.taken,
                std::string("eating: ") + c.name,
                "took " + std::to_string(w.orgs[0].energy - energyBefore) + ", expected " + std::to_string(c.taken));
            check(s, w.energyEaten == c.taken, std::string("energyEaten statistic: ") + c.name);
            check(s, w.totalEnergy() == total && w.totalEnergy() == w.energyIn - w.energyOut,
                std::string("eating moves energy without creating or destroying any: ") + c.name);
        }
    }

    /** Only the head's tile is eaten; the tiles under the rest of the body are untouched. */
    static void testOnlyTheHeadEats(Stats& s) {
        World w = makeBase();
        const size_t K = findWorm(w, 3);
        check(s, K != w.orgs.size(), "setup: found a worm with >= 3 segments");
        if (K == w.orgs.size()) return;
        Organism& o = w.orgs[K];
        const std::vector<int> cells(o.body.begin(), o.body.end());
        for (int c : cells) setFood(w, size_t(c), 50);
        setEnergy(w, o, 1);
        o.genome.biteSize = 7;

        w.eatPhase();

        check(s, w.grid.food[size_t(cells[0])] == 43, "the head's tile lost exactly one bite");
        bool rest = true;
        for (size_t i = 1; i < cells.size(); ++i) rest = rest && (w.grid.food[size_t(cells[i])] == 50);
        check(s, rest, "tiles under the rest of the body are untouched");
        check(s, o.energy == 8, "the worm gained exactly one bite");
    }

    /** A dead worm does not eat. */
    static void testDeadWormsDoNotEat(Stats& s) {
        World w = makeBase();
        const size_t K = findWorm(w, 1);
        check(s, K != w.orgs.size(), "setup: found a living worm");
        if (K == w.orgs.size()) return;
        const size_t head = size_t(w.orgs[K].body.front());
        w.kill(w.orgs[K]);
        setFood(w, head, 60);

        w.eatPhase();

        check(s, w.grid.food[head] == 60, "a dead worm's former tile is not eaten");
        check(s, w.orgs[K].energy == 0, "a dead worm gains no energy");
    }

    /** biteSize is drawn from BITE_MIN..BITE_MAX, and both ends of the range occur. */
    static void testBiteGeneRange(Stats& s) {
        std::mt19937 rng(99);
        int lo = 1 << 30, hi = 0;
        for (int i = 0; i < 2000; ++i) {
            const Genome g = Genome::random(rng);
            lo = std::min(lo, int(g.biteSize));
            hi = std::max(hi, int(g.biteSize));
        }
        check(s, lo >= Tune::BITE_MIN && hi <= Tune::BITE_MAX, "random biteSize stays within BITE_MIN..BITE_MAX",
            "saw " + std::to_string(lo) + ".." + std::to_string(hi));
        check(s, lo == Tune::BITE_MIN && hi == Tune::BITE_MAX, "both ends of the bite range occur in 2000 draws");
    }

    /** In a live world worms really eat, gain energy, and the books still balance. */
    static void testEatingInARunningWorld(Stats& s) {
        World w(80, 60);
        w.seedFood(Tune::FOOD_SEED_PERCENT);
        w.spawnRandom(40);
        const int64_t wormsBefore = w.wormEnergy();
        for (int t = 0; t < 400; ++t) w.step();
        check(s, w.energyEaten > 0, "worms ate something in 400 ticks");
        check(s, w.wormEnergy() > wormsBefore, "the worms' combined energy rose from eating");
        check(s, w.wormEnergy() + w.foodEnergy() == w.totalEnergy(), "wormEnergy + foodEnergy == totalEnergy");
        check(s, w.totalEnergy() == w.energyIn - w.energyOut, "the ledger still balances after 400 ticks of eating");
    }

    /**
     * @brief One single-cell worm in the middle of an empty 20x20 world, full of energy, with the
     *        given growth settings (growInterval 1, so only energy and length decide).
     */
    static World makeGrower(int targetLength, int thresholdPercent) {
        World w(20, 20);
        Genome g;
        g.targetLength = uint8_t(targetLength);
        g.growInterval = 1;
        g.growThreshold = uint8_t(thresholdPercent);
        w.spawn(w.grid.idx(10, 10), g);
        setEnergy(w, w.orgs[0], w.energyCapacity(w.orgs[0]));
        return w;
    }

    /** @brief Make worm 1 take one step straight ahead, exactly as if it had won its contest. */
    static void stepStraight(World& w) {
        Organism& o = w.orgs[0];
        const Intent in{ 1u, 0, w.grid.neighbor(o.body.front(), o.facing), 0.0f };
        w.applyMove(o, 1u, in);
    }

    /** Growing costs energy: the pool is drawn down, the cost is destroyed, and the books balance. */
    static void testGrowthCostsEnergy(Stats& s) {
        World w = makeGrower(5, 0);
        const int before = w.orgs[0].energy;
        stepStraight(w);
        check(s, w.orgs[0].body.size() == 2, "a rich worm grows by one segment when it steps");
        check(s, w.orgs[0].energy == before - Tune::GROW_COST, "growing took exactly GROW_COST from the pool");
        check(s, w.energyOut == Tune::GROW_COST && w.segmentsGrown == 1, "the cost is counted as destroyed energy");
        check(s, w.totalEnergy() == w.energyIn - w.energyOut, "the ledger balances after a growth step");
    }

    /** A worm must keep at least 1 energy: with exactly GROW_COST it moves but does not grow. */
    static void testCannotGrowWithoutEnoughEnergy(Stats& s) {
        World w = makeGrower(5, 0);
        setEnergy(w, w.orgs[0], Tune::GROW_COST);
        const int head = w.orgs[0].body.front();
        stepStraight(w);
        check(s, w.orgs[0].body.size() == 1, "a worm with only GROW_COST energy does not grow");
        check(s, w.orgs[0].body.front() != head, "...but it still moves");
        check(s, w.orgs[0].energy == Tune::GROW_COST && w.segmentsGrown == 0, "...and pays nothing");

        World r = makeGrower(5, 0);
        setEnergy(r, r.orgs[0], Tune::GROW_COST + 1);
        stepStraight(r);
        check(s, r.orgs[0].body.size() == 2 && r.orgs[0].energy == 1, "with GROW_COST + 1 it grows and is left with exactly 1");
    }

    /** growThreshold: energy must be at least that percent of capacity. */
    static void testGrowthThreshold(Stats& s) {
        World full = makeGrower(5, 100);                       // must be completely full
        const int cap = full.energyCapacity(full.orgs[0]);
        stepStraight(full);
        check(s, full.orgs[0].body.size() == 2, "threshold 100%: a full worm grows");

        World nearly = makeGrower(5, 100);
        setEnergy(nearly, nearly.orgs[0], cap - 1);
        stepStraight(nearly);
        check(s, nearly.orgs[0].body.size() == 1, "threshold 100%: a worm one short of full does not grow");

        World half = makeGrower(5, 50);
        setEnergy(half, half.orgs[0], cap / 2 - 1);
        stepStraight(half);
        check(s, half.orgs[0].body.size() == 1, "threshold 50%: just under half does not grow");
    }

    /** The timer still limits growth rate, and the target length still caps it. */
    static void testGrowthTimerAndTargetLength(Stats& s) {
        World w = makeGrower(5, 0);
        w.orgs[0].genome.growInterval = 2;
        w.tick = 1;
        stepStraight(w);
        check(s, w.orgs[0].body.size() == 1, "off the growInterval tick the worm does not grow");
        w.tick = 2;
        stepStraight(w);
        check(s, w.orgs[0].body.size() == 2, "on a growInterval tick it grows");

        World done = makeGrower(1, 0);
        stepStraight(done);
        check(s, done.orgs[0].body.size() == 1 && done.segmentsGrown == 0, "a worm at its target length never grows");
    }

    /** growThreshold is drawn from GROW_THRESHOLD_MIN..MAX, and both ends occur. */
    static void testGrowThresholdGeneRange(Stats& s) {
        std::mt19937 rng(123);
        int lo = 1000, hi = -1;
        for (int i = 0; i < 2000; ++i) {
            const Genome g = Genome::random(rng);
            lo = std::min(lo, int(g.growThreshold));
            hi = std::max(hi, int(g.growThreshold));
        }
        check(s, lo >= Tune::GROW_THRESHOLD_MIN && hi <= Tune::GROW_THRESHOLD_MAX, "random growThreshold stays within its range",
            "saw " + std::to_string(lo) + ".." + std::to_string(hi));
        check(s, lo == Tune::GROW_THRESHOLD_MIN && hi == Tune::GROW_THRESHOLD_MAX, "both ends of the range occur in 2000 draws");
    }

    /** In a live world worms grow, never past their target, and every segment was paid for. */
    static void testGrowthInARunningWorld(Stats& s) {
        World w(80, 60);
        w.seedFood(Tune::FOOD_SEED_PERCENT);
        w.spawnRandom(40);
        for (int t = 0; t < 300; ++t) w.step();
        bool withinTarget = true;
        for (const Organism& o : w.orgs) if (o.alive) withinTarget = withinTarget && int(o.body.size()) <= o.genome.targetLength;
        check(s, w.segmentsGrown > 0, "worms grew in 300 ticks");
        check(s, withinTarget, "no worm is longer than its target length");
        check(s, w.energyOut >= int64_t(w.segmentsGrown) * Tune::GROW_COST, "every segment grown was paid for in the ledger");
        check(s, w.totalEnergy() == w.energyIn - w.energyOut, "the ledger balances after 300 ticks of growth");
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
        testEatingMovesEnergyFromTileToWorm(s);
        testOnlyTheHeadEats(s);
        testDeadWormsDoNotEat(s);
        testBiteGeneRange(s);
        testEatingInARunningWorld(s);
        testGrowthCostsEnergy(s);
        testCannotGrowWithoutEnoughEnergy(s);
        testGrowthThreshold(s);
        testGrowthTimerAndTargetLength(s);
        testGrowThresholdGeneRange(s);
        testGrowthInARunningWorld(s);
    }

} // namespace th