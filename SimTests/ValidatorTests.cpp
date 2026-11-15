/**
 * @file ValidatorTests.cpp
 * @brief Tests of World::validate() itself. A checker that has never been seen
 *        failing cannot be trusted, so this file proves three things:
 *          1. healthy worlds never trip it (no false alarms),
 *          2. calling it never changes a simulation (it is read-only),
 *          3. deliberately corrupted worlds are always caught, with a useful message.
 * Dependencies: TestHarness.h, World.h. Called by: main.cpp.
 */
#include "TestHarness.h"
#include <algorithm>
#include <functional>
#include <vector>

namespace th {

    /** 1. Many seeds and grid sizes, validated after every tick and every spawn/clear. */
    static void testHealthyWorlds(Stats& s) {
        long validations = 0;
        bool allClean = true;
        for (int seed = 1; seed <= 25 && allClean; ++seed) {
            const bool small = (seed % 2) != 0;
            World w(small ? 30 : 60, small ? 20 : 40);
            w.rng.seed(uint32_t(seed));
            w.spawnRandom(seed % 3 == 0 ? 120 : 40);          // some very crowded worlds

            auto verify = [&](const char* when, int tick) {
                ++validations;
                std::string e = w.validate();
                if (e.empty()) return;
                allClean = false;
                check(s, false, "healthy world stays valid",
                    "seed " + std::to_string(seed) + ", " + when + ", tick " + std::to_string(tick) + ": " + e);
                };

            verify("after spawn", 0);
            for (int t = 0; t < 800 && allClean; ++t) {
                w.step();
                verify("after step", t);
                if (t % 100 == 50) { w.spawnRandom(5); verify("after respawn", t); }
                if (t == 400 && seed % 5 == 0) { w.clear(); w.spawnRandom(30); verify("after clear", t); }
            }
        }
        check(s, allClean, "healthy worlds: no violations in " + std::to_string(validations) + " validations");
    }

    /** 2. Validation must not consume RNG or change state. */
    static void testValidateIsReadOnly(Stats& s) {
        World a(60, 40), b(60, 40);
        a.spawnRandom(40);
        b.spawnRandom(40);
        for (int t = 0; t < 600; ++t) {
            a.step();
            (void)a.validate();       // a is validated every tick, b never
            b.step();
        }
        check(s, fingerprint(a) == fingerprint(b), "validate() does not change the simulation");
    }

    /** One deliberate corruption and the text its message must contain. */
    struct Case {
        const char* name;
        std::string expect;
        std::function<void(World&)> corrupt;
    };

    /** 3. Every corruption must be detected, with a message naming the problem. */
    static void testCorruptionIsDetected(Stats& s) {
        World base(30, 20);
        base.rng.seed(7);
        base.spawnRandom(10);
        for (int t = 0; t < 60; ++t) base.step();

        // Pick a victim worm with a body of at least 4 segments.
        size_t K = base.orgs.size();
        for (size_t i = 0; i < base.orgs.size(); ++i)
            if (base.orgs[i].alive && base.orgs[i].body.size() >= 4) { K = i; break; }
        check(s, K != base.orgs.size(), "corruption setup: found a worm with >= 4 segments");
        if (K == base.orgs.size()) return;
        check(s, base.validate().empty(), "corruption setup: base world is valid", base.validate());

        // Another living worm, a free cell, and a free cell far from the victim.
        size_t other = K;
        for (size_t i = 0; i < base.orgs.size(); ++i)
            if (i != K && base.orgs[i].alive) { other = i; break; }
        const int cells = base.grid.w * base.grid.h;
        int emptyCell = -1, farCell = -1;
        for (int i = 0; i < cells; ++i)         if (base.grid.type[size_t(i)] == T_EMPTY) { emptyCell = i; break; }
        for (int i = cells - 1; i >= 0; --i)    if (base.grid.type[size_t(i)] == T_EMPTY) { farCell = i; break; }

        // Whether a stolen cell is reported as "already occupied" depends on whether the
        // thief's victim comes earlier in organism order than the worm being damaged.
        const std::string stolenExpect = (other > K) ? "not adjacent" : "already occupied";

        const std::vector<Case> cases = {
            { "hp shorter than body",            "but hp has",               [&](World& w) { w.orgs[K].hp.pop_back(); } },
            { "two segments share a cell",       "already occupied",         [&](World& w) { w.orgs[K].body[1] = w.orgs[K].body[0]; } },
            { "another worm's cell stolen",      stolenExpect,               [&](World& w) { w.orgs[K].body[2] = w.orgs[other].body[0]; } },
            { "segment jumps to a far cell",     "not adjacent",             [&](World& w) { w.orgs[K].body[2] = farCell; } },
            { "body index off the grid",         "outside the grid",         [&](World& w) { w.orgs[K].body[1] = 99999; } },
            { "negative body index",             "outside the grid",         [&](World& w) { w.orgs[K].body[0] = -3; } },
            { "wrong grid orgId",                "grid orgId",               [&](World& w) { w.grid.orgId[size_t(w.orgs[K].body[1])] = 99; } },
            { "wrong grid type",                 "genome expects",           [&](World& w) {
                const size_t c = size_t(w.orgs[K].body[1]);
                w.grid.type[c] = (w.grid.type[c] == T_OFFENSE) ? uint8_t(T_STORAGE) : uint8_t(T_OFFENSE); } },
            { "stale hp mirror",                 "hp mirror",                [&](World& w) { w.grid.hp[size_t(w.orgs[K].body[0])] ^= 1; } },
            { "segment at 0 hp",                 "hp > 0",                   [&](World& w) { w.orgs[K].hp[2] = 0; } },
            { "orphan occupied cell",            "no living organism owns",  [&](World& w) { w.grid.type[size_t(emptyCell)] = T_MOVER; } },
            { "stale orgId on empty cell",       "stale orgId/hp",           [&](World& w) { w.grid.orgId[size_t(emptyCell)] = 3; } },
            { "stale hp on empty cell",          "stale orgId/hp",           [&](World& w) { w.grid.hp[size_t(emptyCell)] = 5; } },
            { "killed worm, grid not cleared",   "no living organism owns",  [&](World& w) {
                const size_t c = size_t(w.orgs[K].body[1]);
                w.kill(w.orgs[K]);
                w.grid.type[c] = T_HEAD; w.grid.orgId[c] = uint32_t(K + 1); w.grid.hp[c] = 5; } },
            { "marked dead without kill()",      "dead but still has",       [&](World& w) { w.orgs[K].alive = false; } },
            { "alive with empty body",           "empty body",               [&](World& w) { w.orgs[K].body.clear(); w.orgs[K].hp.clear(); } },
            { "deaths counter wrong",            "deaths counter",           [&](World& w) { ++w.deaths; } },
            { "genome.period = 0",               "genome.period",            [&](World& w) { w.orgs[K].genome.period = 0; } },
            { "genome.period = 5",               "genome.period",            [&](World& w) { w.orgs[K].genome.period = 5; } },
            { "genome.growInterval = 0",         "growInterval",             [&](World& w) { w.orgs[K].genome.growInterval = 0; } },
            { "genome.targetLength = 0",         "targetLength is 0",        [&](World& w) { w.orgs[K].genome.targetLength = 0; } },
            { "body longer than targetLength",   "more than genome.target",  [&](World& w) { w.orgs[K].genome.targetLength = 2; } },
            { "tail type = empty",               "invalid cell type",        [&](World& w) { w.orgs[K].genome.tailType = T_EMPTY; } },
            { "facing out of range",             "facing",                   [&](World& w) { w.orgs[K].facing = 9; } },
            { "grid array wrong size",           "w*h elements",             [&](World& w) { w.grid.hp.pop_back(); } },
        };

        for (const Case& c : cases) {
            World w = base;                       // fresh copy of the healthy world
            c.corrupt(w);
            const std::string e = w.validate();
            check(s, !e.empty(), std::string("detects: ") + c.name, "(validate() returned empty)");
            if (!e.empty())
                check(s, e.find(c.expect) != std::string::npos,
                    std::string("message for: ") + c.name,
                    "expected text \"" + c.expect + "\" but got: " + e);
        }

        // Legitimate operations must NOT be flagged.
        World w = base;
        w.kill(w.orgs[K]);
        check(s, w.validate().empty(), "a legitimate kill() leaves a valid world", w.validate());
    }

    void runValidatorTests(Stats& s) {
        std::printf("[validator]\n");
        testHealthyWorlds(s);
        testValidateIsReadOnly(s);
        testCorruptionIsDetected(s);
    }

} // namespace th