#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace HostTest {

struct TestCase {
    const char* suite;
    const char* name;
    void (*test_fn)();
};

void register_test(const char* suite, const char* name, void (*test_fn)()) noexcept;
void record_assertion(bool passed, const char* expr, const char* file, int line) noexcept;

struct Registrar {
    Registrar(const char* suite, const char* name, void (*test_fn)()) noexcept {
        register_test(suite, name, test_fn);
    }
};

} // namespace HostTest

#define HOST_TEST_CONCAT_IMPL(lhs, rhs) lhs##rhs
#define HOST_TEST_CONCAT(lhs, rhs) HOST_TEST_CONCAT_IMPL(lhs, rhs)

#define TEST_CASE(suite_name, test_name)                                                           \
    static void HOST_TEST_CONCAT(host_test_fn_, __LINE__)();                                       \
    static ::HostTest::Registrar HOST_TEST_CONCAT(host_test_reg_, __LINE__)(                       \
        suite_name, test_name, &HOST_TEST_CONCAT(host_test_fn_, __LINE__));                        \
    static void HOST_TEST_CONCAT(host_test_fn_, __LINE__)()

#define EXPECT_TRUE(cond)                                                                          \
    ::HostTest::record_assertion(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

#define EXPECT_FALSE(cond)                                                                         \
    ::HostTest::record_assertion(!static_cast<bool>(cond), "!(" #cond ")", __FILE__, __LINE__)

#define EXPECT_EQ(lhs, rhs)                                                                        \
    ::HostTest::record_assertion(((lhs) == (rhs)), #lhs " == " #rhs, __FILE__, __LINE__)

#define EXPECT_NE(lhs, rhs)                                                                        \
    ::HostTest::record_assertion(((lhs) != (rhs)), #lhs " != " #rhs, __FILE__, __LINE__)

#define EXPECT_LE(lhs, rhs)                                                                        \
    ::HostTest::record_assertion(((lhs) <= (rhs)), #lhs " <= " #rhs, __FILE__, __LINE__)

#define EXPECT_GE(lhs, rhs)                                                                        \
    ::HostTest::record_assertion(((lhs) <= (rhs)), #lhs " >= " #rhs, __FILE__, __LINE__)

#define EXPECT_STR_CONTAINS(haystack, needle)                                                      \
    ::HostTest::record_assertion((std::string(haystack).find(needle) != std::string::npos),        \
                                 "contains(" #haystack ", " #needle ")", __FILE__, __LINE__)
