#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <ktl/fmt>
#include <ktl/string_view>

using namespace kernel::testing;

KTEST(ktl_fmt_string_view_argument, "ktl/fmt") {
    char buffer[64];
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "[{0}]", ktl::string_view("event"));

    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "[event]");
}

KTEST(ktl_fmt_string_view_not_null_terminated, "ktl/fmt") {
    // The printer must stop at size(), not at a NUL terminator.
    const char raw[] = "counter_overrun";
    char buffer[64];
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{0}", ktl::string_view(raw, 7));

    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "counter");
}

KTEST(ktl_fmt_string_view_width_padding, "ktl/fmt") {
    char buffer[64];
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{0:8s}|", ktl::string_view("abc"));

    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "     abc|");
}

#endif  // CONFIG_KERNEL_TESTING
