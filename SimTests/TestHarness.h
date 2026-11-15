#pragma once
/**
 * @file TestHarness.h
 * @brief Minimal test helpers shared by the SimTests project.
 * Dependencies: World.h (simulation only). No raylib, by design: if the
 * simulation ever starts depending on graphics code, this project stops
 * compiling, which is exactly the point.
 * Used by: ValidatorTests.cpp, RegressionTests.cpp, main.cpp.
 */
#include <cstdint>
#include <cstdio>
#include <string>
#include "World.h"

namespace th {

    /** @brief Running totals for one test session. */
    struct Stats {
        int checks = 0;     ///< assertions evaluated
        int failures = 0;   ///< assertions that failed
        int pending = 0;    ///< checks that cannot pass yet (e.g. golden value not recorded)
    };

    /**
     * @brief Record one assertion. Prints a line only on failure.
     * @param s      Session totals to update.
     * @param ok     The condition that must be true.
     * @param what   Short name of the check.
     * @param detail Optional extra text (for example the validator's message).
     */
    inline void check(Stats& s, bool ok, const std::string& what, const std::string& detail = "") {
        ++s.checks;
        if (ok) return;
        ++s.failures;
        std::printf("  FAIL: %s%s%s\n", what.c_str(), detail.empty() ? "" : "  ->  ", detail.c_str());
    }

    /** @brief 16-digit lowercase hex text of a 64-bit value (for messages). */
    inline std::string hex64(uint64_t v) {
        char buf[17];
        std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)v);
        return std::string(buf);
    }

    /** @brief One FNV-1a mixing step (64-bit). */
    inline uint64_t fnv1a(uint64_t h, uint64_t v) {
        h ^= v;
        h *= 1099511628211ULL;
        return h;
    }

    /**
     * @brief 64-bit fingerprint of everything visible in the world: every cell's
     *        type, orgId and hp, plus tick, contests, deaths and the alive count.
     * Two worlds with the same fingerprint are, for practical purposes, identical.
     * @note Reads only. Not stable across compilers or standard libraries (see I11);
     *       compare fingerprints only between runs of the same build toolchain.
     */
    inline uint64_t fingerprint(const World& w) {
        uint64_t h = 1469598103934665603ULL;
        for (uint8_t t : w.grid.type)  h = fnv1a(h, t);
        for (uint32_t o : w.grid.orgId) h = fnv1a(h, o);
        for (uint8_t p : w.grid.hp)    h = fnv1a(h, p);
        h = fnv1a(h, w.tick);
        h = fnv1a(h, w.contests);
        h = fnv1a(h, w.deaths);
        h = fnv1a(h, uint64_t(w.aliveCount()));
        return h;
    }

    // Test groups (each defined in its own .cpp, called from main.cpp).
    void runValidatorTests(Stats& s);
    void runEnergyTests(Stats& s);
    void runRegressionTests(Stats& s);

} // namespace th