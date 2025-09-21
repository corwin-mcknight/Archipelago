#include <kernel/log.h>
#include <kernel/testing/testing.h>

using namespace kernel::testing;

int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

KTEST(factorial_test, "factorials") { KTEST_REQUIRE_EQUAL(factorial(0), 1); }
KTEST(factorial_test_2, "factorials") { KTEST_REQUIRE_EQUAL(factorial(5), 120); }