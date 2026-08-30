#include <kernel/testing/testing.h>
#include <kernel/time.h>
#include <stdint.h>

using namespace kernel::testing;

KTEST_MODULE("core/time");

KTEST_CASE(time_nanoseconds_round_up_without_overflow) {
    KTEST_EXPECT_EQUAL(kernel::time_detail::divide_ceil(0, 1000), static_cast<uint64_t>(0));
    KTEST_EXPECT_EQUAL(kernel::time_detail::divide_ceil(1, 1000), static_cast<uint64_t>(1));
    KTEST_EXPECT_EQUAL(kernel::time_detail::divide_ceil(1000, 1000), static_cast<uint64_t>(1));
    KTEST_EXPECT_EQUAL(kernel::time_detail::divide_ceil(1001, 1000), static_cast<uint64_t>(2));
    KTEST_EXPECT_EQUAL(kernel::time_detail::divide_ceil(UINT64_MAX, 1000), UINT64_MAX / 1000 + 1);
}
