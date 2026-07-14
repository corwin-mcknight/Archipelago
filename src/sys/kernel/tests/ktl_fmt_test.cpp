#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <ktl/fmt>
#include <ktl/maybe>
#include <ktl/string_view>

using namespace kernel::testing;

KTEST_MODULE("ktl/fmt");

KTEST_CASE(ktl_fmt_string_arguments) {
    char buffer[64];
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "[{0}]", ktl::string_view("event"));
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "[event]");

    // The printer must stop at size(), not at a NUL terminator.
    const char raw[] = "counter_overrun";
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{0}", ktl::string_view(raw, 7));
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "counter");

    const char* s = "hi";
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "[{0}]", s);
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "[hi]");
}

KTEST_CASE(ktl_fmt_width_alignment_and_flags) {
    char buffer[64];
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{0:8s}|", ktl::string_view("abc"));
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "     abc|");

    // A spec ending right after the width ("{0:8}") must close at its own '}',
    // not swallow output until the next '}'.
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{0:8}|", 42);
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "      42|");

    // '-' left-aligns, '0' zero-pads.
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{0:-4}|", 42);
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "42  |");
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{0:04}", 42);
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "0042");
}

KTEST_CASE(ktl_fmt_integer_bases_and_char) {
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

    // 'c' prints the raw character instead of the integer value.
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{0:c}", 'A');
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "A");
}

KTEST_CASE(ktl_fmt_maybe_argument) {
    char buffer[64];
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{0}", ktl::maybe<int>(42));
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "42");

    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{0}", ktl::maybe<int>(ktl::nothing));
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "<<ktl::nothing_t>>");
}

KTEST_CASE(ktl_fmt_buffer_bounds) {
    // An empty maybe renders "<<ktl::nothing_t>>"; a small buffer must clip it, not overrun.
    char buffer[8];
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{0}", ktl::maybe<int>(ktl::nothing));
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "<<ktl::");

    // buffer_max == 0 must be a no-op, not a wrapped bound.
    char canary = 'x';
    ktl::format::format_to_buffer_raw(&canary, 0, "{0}", 42);
    KTEST_EXPECT_EQUAL(canary, 'x');
}

KTEST_CASE(ktl_fmt_escapes_and_bad_index) {
    char buffer[32];
    // Doubled braces emit literal braces.
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{{{0}}}", 7);
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "{7}");

    // An argument index past the pack renders a placeholder, not garbage.
    ktl::format::format_to_buffer_raw(buffer, sizeof(buffer), "{5}", 1);
    KTEST_EXPECT_TRUE(ktl::string_view(buffer) == "<invalid argument>");
}

#endif  // CONFIG_KERNEL_TESTING
