#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <ktl/fixed_string>
#include <ktl/string_view>

using namespace kernel::testing;

KTEST_MODULE("ktl/fixed_string");

KTEST_CASE(ktl_fixed_string_view_fills_buffer_keeps_terminator) {
    // A view as long as the buffer must be truncated to leave room for the NUL,
    // so length()/c_str() never read past the array.
    ktl::fixed_string<4> s(ktl::string_view("abcd", 4));

    KTEST_EXPECT_TRUE(s.length() == 3);
    KTEST_EXPECT_TRUE(ktl::string_view(s.c_str()) == "abc");
}

#endif  // CONFIG_KERNEL_TESTING
