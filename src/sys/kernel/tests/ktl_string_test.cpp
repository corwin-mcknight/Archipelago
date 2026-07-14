#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <stddef.h>

#include <ktl/string>

using namespace kernel::testing;

KTEST_MODULE("ktl/string");

KTEST_CASE(ktl_strlen_counts_to_first_null) {
    const char empty[] = "";
    KTEST_EXPECT_TRUE(ktl::strlen(empty) == 0);

    // Counting stops at the first NUL and never reads or touches the bytes after it.
    char buffer[] = {'k', 'e', 'r', '\0', 'X', 'Y'};
    KTEST_EXPECT_TRUE(ktl::strlen(buffer) == 3);
    KTEST_EXPECT_EQUAL(buffer[4], 'X');
    KTEST_EXPECT_EQUAL(buffer[5], 'Y');

    // The template works over wider character types too.
    const char16_t wide[] = {u'A', u'B', u'C', u'\0', u'Z'};
    KTEST_EXPECT_TRUE(ktl::strlen(wide) == 3);
}

#endif  // CONFIG_KERNEL_TESTING
