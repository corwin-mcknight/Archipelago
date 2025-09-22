#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <stddef.h>

#include <ktl/string>

using namespace kernel::testing;

KTEST(ktl_strlen_empty_string, "ktl/string") {
    const char buffer[] = "";
    KTEST_EXPECT_TRUE(ktl::strlen(buffer) == 0);
}

KTEST(ktl_strlen_stops_at_first_null, "ktl/string") {
    char buffer[] = {'k', 'e', 'r', '\0', 'X', 'Y'};

    size_t len = ktl::strlen(buffer);

    KTEST_EXPECT_TRUE(len == 3);
    KTEST_EXPECT_EQUAL(buffer[4], 'X');
    KTEST_EXPECT_EQUAL(buffer[5], 'Y');
}

KTEST(ktl_strlen_supports_wide_char_type, "ktl/string") {
    const char16_t buffer[] = {u'A', u'B', u'C', u'\0', u'Z'};

    size_t len = ktl::strlen(buffer);

    KTEST_EXPECT_TRUE(len == 3);
}

#endif  // CONFIG_KERNEL_TESTING