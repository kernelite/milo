#include "mocks/host_hal.hpp"
#include "test_framework.hpp"

#include <cstdio>
#include <vector>

namespace {

struct RunnerState {
    std::vector<HostTest::TestCase> cases;
    size_t failures{0U};
};

RunnerState& runner_state() {
    static RunnerState state{};
    return state;
}

} // namespace

namespace HostTest {

void register_test(const char* suite, const char* name, void (*test_fn)()) noexcept {
    try {
        runner_state().cases.push_back(TestCase{suite, name, test_fn});
    } catch (...) {
        std::abort();
    }
}

void record_assertion(bool passed, const char* expr, const char* file, int line) noexcept {
    if (!passed) {
        (void)std::fprintf(stderr, "    [FAIL] %s:%d: %s\n", file, line, expr);
        ++runner_state().failures;
    }
}

} // namespace HostTest

int main() {
    HostHAL::reset();

    size_t passed_suites = 0U;
    size_t failed_suites = 0U;

    const auto& cases = runner_state().cases;
    (void)std::printf("[HOST-TEST] Running %zu unit test cases...\n", cases.size());

    for (const auto& test_case : cases) {
        HostHAL::reset();
        const size_t failures_before = runner_state().failures;
        test_case.test_fn();
        const size_t failures_after = runner_state().failures;

        if (failures_after == failures_before) {
            (void)std::printf("  [PASS] %s.%s\n", test_case.suite, test_case.name);
            ++passed_suites;
        } else {
            (void)std::printf("  [FAIL] %s.%s (%zu assertion(s) failed)\n", test_case.suite,
                              test_case.name, failures_after - failures_before);
            ++failed_suites;
        }
    }

    (void)std::printf("[HOST-TEST] Summary: %zu passed, %zu failed.\n", passed_suites,
                      failed_suites);
    return (failed_suites == 0U) ? 0 : 1;
}
