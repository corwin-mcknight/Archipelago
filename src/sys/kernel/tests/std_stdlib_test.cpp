#include <kernel/testing/testing.h>
#include <std/stdlib.h>
#include <stddef.h>

#if CONFIG_KERNEL_TESTING

using namespace kernel::testing;

KTEST(std_atoi_positive_negative, "std/stdlib") {
    KTEST_EXPECT_EQUAL(atoi("0"), 0);
    KTEST_EXPECT_EQUAL(atoi("12345"), 12345);
    KTEST_EXPECT_EQUAL(atoi("-4096"), -4096);
}

KTEST(std_itoa_zero, "std/stdlib") {
    char buffer[4] = {0};

    itoa(0, buffer, 10);

    KTEST_EXPECT_EQUAL(buffer[0], '0');
    KTEST_EXPECT_EQUAL(buffer[1], '\0');
}

KTEST(std_itoa_base_variants, "std/stdlib") {
    char buffer10[32] = {};
    char buffer16[32] = {};

    itoa(987654321ULL, buffer10, 10);
    itoa(0xDEADBEEF, buffer16, 16);

    KTEST_EXPECT_EQUAL(buffer10[0], '9');
    KTEST_EXPECT_EQUAL(buffer10[8], '1');
    KTEST_EXPECT_EQUAL(buffer10[9], '\0');

    KTEST_EXPECT_EQUAL(buffer16[0], 'D');
    KTEST_EXPECT_EQUAL(buffer16[1], 'E');
    KTEST_EXPECT_EQUAL(buffer16[7], 'F');
    KTEST_EXPECT_EQUAL(buffer16[8], '\0');
}

#endif