/**
 * @file RegressionTests.cpp
 * @brief "Did that change alter behavior?" tests.
 *
 * Each scenario in SCENARIOS is a fixed world (size, seed, worm count, tick
 * count). For every scenario this file checks:
 *   1. Validity: the world passes World::validate() after every tick.
 *   2. Determinism: running it twice gives an identical world. This must hold on
 *      every toolchain.
 *   3. Golden fingerprint: the result matches a value recorded from THIS
 *      toolchain (see SCENARIOS below).
 *
 * Why two scenarios: the "standard" one has few contests, so small balance
 * changes (for example Tune::JITTER 0.5 -> 0.6) never flip a winner and leave its
 * fingerprint unchanged. The "crowded" one is dense with contests and reacts to
 * all the small changes tried (JITTER, SIZE_BONUS, DEFENSE).
 *
 * Dependencies: TestHarness.h, World.h. Called by: main.cpp.
 */
#include "TestHarness.h"

namespace th {

    /** @brief One fixed scenario and its recorded fingerprint. */
    struct Scenario {
        const char* name;
        int         width, height;
        int         worms;      ///< passed to World::spawnRandom
        int         ticks;
        uint64_t    golden;     ///< recorded fingerprint; 0 = not recorded yet
    };

    /**
     * @brief The scenario table. World seed is always 12345.
     *
     * HOW TO RECORD: run this project once. For each scenario it prints a line
     * "Set golden = 0x...ULL". Paste each value into the last column below and rebuild.
     * Re-record deliberately, in the same commit, after any change that is MEANT to
     * alter simulation behavior (new rules, rebalanced Tune values, new RNG draws).
     * If a change that was supposed to be a pure refactor fails these tests, the
     * refactor changed behavior.
     *
     * @warning A golden value is valid only for the compiler + standard library that
     *          recorded it (I11). Record Debug and Release builds separately only if
     *          they disagree (they should not; if they do, that is a finding).
     */
    static const Scenario SCENARIOS[] = {
        // name        w   h  worms ticks  golden (0 = not recorded)
        { "standard", 60, 40,   40,  600,  0xe487e1defa52e738 },
        { "crowded",  30, 20,  120,  800,  0x5ef96eb43fbad5eb },
    };

    /**
     * @brief Run one scenario from scratch, validating after every tick.
     * @param s      Session totals (a validation failure is recorded here).
     * @param sc     The scenario to run.
     * @param clean  Set to false if validate() ever failed.
     * @return The fingerprint of the final world.
     */
    static uint64_t runScenario(Stats& s, const Scenario& sc, bool* clean) {
        World w(sc.width, sc.height);
        w.rng.seed(12345);
        w.spawnRandom(sc.worms);
        *clean = true;
        for (int t = 0; t < sc.ticks; ++t) {
            w.step();
            const std::string e = w.validate();
            if (!e.empty()) {
                check(s, false, std::string(sc.name) + ": world stays valid", "tick " + std::to_string(t) + ": " + e);
                *clean = false;
                break;
            }
        }
        std::printf("  %-8s alive %d/%zu, contests %llu, deaths %llu\n", sc.name,
            w.aliveCount(), w.orgs.size(),
            (unsigned long long)w.contests, (unsigned long long)w.deaths);
        return fingerprint(w);
    }

    void runRegressionTests(Stats& s) {
        std::printf("[regression]\n");
        for (const Scenario& sc : SCENARIOS) {
            bool clean1 = true, clean2 = true;
            const uint64_t fp1 = runScenario(s, sc, &clean1);
            const uint64_t fp2 = runScenario(s, sc, &clean2);
            check(s, clean1 && clean2, std::string(sc.name) + ": valid on both runs");
            check(s, fp1 == fp2, std::string(sc.name) + ": same seed gives an identical world (determinism)");

            if (sc.golden == 0) {
                ++s.pending;
                std::printf("  PENDING (%s): not recorded. Set golden = 0x%016llxULL\n",
                    sc.name, (unsigned long long)fp1);
            }
            else {
                check(s, fp1 == sc.golden, std::string(sc.name) + ": matches the recorded golden fingerprint",
                    "expected 0x" + hex64(sc.golden) + ", got 0x" + hex64(fp1));
            }
        }
    }

} // namespace th