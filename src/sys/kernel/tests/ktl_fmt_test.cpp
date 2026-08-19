#include <kernel/testing/testing.h>

#include <ktl/fmt>
#include <ktl/maybe>
#include <ktl/string_view>

using namespace kernel::testing;

KTEST_MODULE("ktl/fmt");

// Format into a cap-byte buffer and expect the result; the plain form uses a roomy 64-byte buffer,
// the _N form pins the capacity for clipping cases.
#define KTEST_EXPECT_FMT_N(cap, expected, ...)                          \
    do {                                                                \
        char _b[cap] = {};                                              \
        ktl::format::format_to_buffer_raw(_b, sizeof(_b), __VA_ARGS__); \
        KTEST_EXPECT_TRUE(ktl::string_view(_b) == (expected));          \
    } while (0)
#define KTEST_EXPECT_FMT(expected, ...) KTEST_EXPECT_FMT_N(64, expected, __VA_ARGS__)

KTEST_CASE(ktl_fmt_string_arguments) {
    KTEST_EXPECT_FMT("[event]", "[{0}]", ktl::string_view("event"));

    // The printer must stop at size(), not at a NUL terminator.
    const char raw[] = "counter_overrun";
    KTEST_EXPECT_FMT("counter", "{0}", ktl::string_view(raw, 7));

    const char* s = "hi";
    KTEST_EXPECT_FMT("[hi]", "[{0}]", s);
}

KTEST_CASE(ktl_fmt_width_alignment_and_flags) {
    KTEST_EXPECT_FMT("     abc|", "{0:8s}|", ktl::string_view("abc"));

    // A spec ending right after the width ("{0:8}") must close at its own '}',
    // not swallow output until the next '}'.
    KTEST_EXPECT_FMT("      42|", "{0:8}|", 42);

    // '-' left-aligns, '0' zero-pads.
    KTEST_EXPECT_FMT("42  |", "{0:-4}|", 42);
    KTEST_EXPECT_FMT("0042", "{0:04}", 42);
}

KTEST_CASE(ktl_fmt_utf8_width_and_truncation) {
    // Width pads by codepoints, not bytes: "héllo" is 6 bytes but 5 columns.
    KTEST_EXPECT_FMT("  h\xc3\xa9llo|", "{0:7}|", ktl::string_view("h\xc3\xa9llo"));
    KTEST_EXPECT_FMT("h\xc3\xa9llo  |", "{0:-7}|", ktl::string_view("h\xc3\xa9llo"));

    // A string wider than its field gets no padding, not width-of-string padding.
    KTEST_EXPECT_FMT("abc|", "{0:2}|", ktl::string_view("abc"));

    // Truncation at the buffer edge must drop a split multibyte sequence, not
    // emit its lead byte as invalid UTF-8.
    KTEST_EXPECT_FMT_N(3, "a", "{0}", ktl::string_view("a\xc3\xa9"));
}

KTEST_CASE(ktl_fmt_integer_bases_and_char) {
    KTEST_EXPECT_FMT("FF", "{0:x}", 255);
    KTEST_EXPECT_FMT("FF", "{0:h}", 255);
    KTEST_EXPECT_FMT("10", "{0:o}", 8);
    KTEST_EXPECT_FMT("101", "{0:b}", 5);
    KTEST_EXPECT_FMT("1000", "{0:p}", 0x1000);

    // 'c' prints the raw character instead of the integer value.
    KTEST_EXPECT_FMT("A", "{0:c}", 'A');

    // The most-negative values have no positive counterpart in their own type;
    // the printer must negate in unsigned space.
    KTEST_EXPECT_FMT("-2147483648", "{0}", -2147483647 - 1);
    KTEST_EXPECT_FMT("-9223372036854775808", "{0}", -9223372036854775807LL - 1);
}

KTEST_CASE(ktl_fmt_buffer_bounds) {
    // A small buffer must clip the output, not overrun.
    KTEST_EXPECT_FMT_N(8, "clipped", "{0}", "clipped string");

    // buffer_max == 0 must be a no-op, not a wrapped bound.
    char canary = 'x';
    ktl::format::format_to_buffer_raw(&canary, 0, "{0}", 42);
    KTEST_EXPECT_EQUAL(canary, 'x');
}

KTEST_CASE(ktl_fmt_escapes_and_bad_index) {
    // Doubled braces emit literal braces.
    KTEST_EXPECT_FMT("{7}", "{{{0}}}", 7);

    // An argument index past the pack renders a placeholder, not garbage.
    KTEST_EXPECT_FMT("<invalid argument>", "{5}", 1);
}
