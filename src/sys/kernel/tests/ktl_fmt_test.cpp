#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <ktl/fmt>
#include <ktl/maybe>
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

KTEST(ktl_fmt_width_without_specifier, "ktl/fmt") {
    // A spec ending right after the width ("{0:8}") must close at its own '}',
    // not swallow output until the next '}'.
    char buffer[64];
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{0:8}|", 42);

    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "      42|");
}

KTEST(ktl_fmt_maybe_argument, "ktl/fmt") {
    char buffer[64];
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{0}", ktl::maybe<int>(42));
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "42");

    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{0}", ktl::maybe<int>(ktl::nothing));
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "<<ktl::nothing_t>>");
}

KTEST(ktl_fmt_maybe_truncates_at_buffer_end, "ktl/fmt") {
    // An empty maybe renders "<<ktl::nothing_t>>"; a small buffer must clip it, not overrun.
    char buffer[8];
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{0}", ktl::maybe<int>(ktl::nothing));
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "<<ktl::");
}

KTEST(ktl_fmt_zero_sized_buffer, "ktl/fmt") {
    // buffer_max == 0 must be a no-op, not a wrapped bound.
    char canary = 'x';
    ktl::format::format_to_buffer_raw(&canary, 0, "{0}", 42);
    KTEST_EXPECT_EQUAL(canary, 'x');
}

#endif  // CONFIG_KERNEL_TESTING
