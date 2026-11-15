/**
 * @file main.cpp
 * @brief Entry point of the SimTests console project: runs every test group and
 *        reports a summary. Exit code 0 = all checks passed, 1 = a check failed.
 * Pending checks (such as an unrecorded golden fingerprint) do not fail the run
 * but are reported loudly.
 * Dependencies: TestHarness.h. Builds and runs with no graphics library.
 */
#include <cstdio>
#include "TestHarness.h"

int main() {
    th::Stats stats;
    th::runValidatorTests(stats);
    th::runEnergyTests(stats);
    th::runRegressionTests(stats);

    std::printf("\n%d checks, %d failed, %d pending\n", stats.checks, stats.failures, stats.pending);
    std::printf("%s\n", stats.failures ? "RESULT: FAILED"
        : (stats.pending ? "RESULT: PASSED (with pending items)" : "RESULT: PASSED"));
    return stats.failures ? 1 : 0;
}