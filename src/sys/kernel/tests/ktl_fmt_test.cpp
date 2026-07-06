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

KTEST(ktl_fmt_integer_bases, "ktl/fmt") {
    char buffer[64];
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{0:x}", 255);
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "FF");
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{0:h}", 255);
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "FF");
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{0:o}", 8);
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "10");
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{0:b}", 5);
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "101");
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{0:p}", 0x1000);
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "1000");
}

KTEST(ktl_fmt_char_specifier, "ktl/fmt") {
    char buffer[8];
    // 'c' prints the raw character instead of the integer value.
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{0:c}", 'A');
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "A");
}

KTEST(ktl_fmt_cstring_argument, "ktl/fmt") {
    char buffer[16];
    const char* s = "hi";
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "[{0}]", s);
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "[hi]");
}

KTEST(ktl_fmt_escapes_flags_and_bad_index, "ktl/fmt") {
    char buffer[32];
    // Doubled braces emit literal braces.
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{{{0}}}", 7);
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "{7}");

    // '-' left-aligns, '0' zero-pads.
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{0:-4}|", 42);
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "42  |");
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{0:04}", 42);
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "0042");

    // An argument index past the pack renders a placeholder, not garbage.
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{5}", 1);
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "<invalid argument>");
}

#endif  // CONFIG_KERNEL_TESTING
