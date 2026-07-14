#include <kernel/testing/testing.h>
#include <std/ctype.h>

#if CONFIG_KERNEL_TESTING

using namespace kernel::testing;

KTEST_MODULE("std/ctype");

KTEST_CASE(std_ctype_classification) {
    KTEST_EXPECT_TRUE(isalpha('a'));
    KTEST_EXPECT_TRUE(isalpha('Z'));
    KTEST_EXPECT_FALSE(isalpha('0'));
    KTEST_EXPECT_FALSE(isalpha('#'));

    KTEST_EXPECT_TRUE(isdigit('0'));
    KTEST_EXPECT_TRUE(isdigit('9'));
    KTEST_EXPECT_FALSE(isdigit('a'));
    KTEST_EXPECT_FALSE(isdigit('-'));

    KTEST_EXPECT_TRUE(islower('m'));
    KTEST_EXPECT_FALSE(islower('M'));
    KTEST_EXPECT_TRUE(isupper('X'));
    KTEST_EXPECT_FALSE(isupper('x'));
}

#endif
