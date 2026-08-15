#include <kernel/testing/testing.h>
#include <std/string.h>
#include <stddef.h>

using namespace kernel::testing;

KTEST_MODULE("std/string");

KTEST_CASE(std_memcpy_and_memmove) {
    // memcpy into a distinct buffer copies every byte and returns dest.
    char source[]       = {'A', 'B', 'C', 'D', 'E'};
    char destination[5] = {};
    void* result        = memcpy(destination, source, sizeof(source));
    KTEST_EXPECT_TRUE(result == destination);
    for (size_t i = 0; i < sizeof(source); ++i) { KTEST_EXPECT_EQUAL(destination[i], source[i]); }

    // memmove with a forward-overlapping region.
    char forward[] = {'0', '1', '2', '3', '4', '5'};
    result         = memmove(forward + 2, forward, 4);
    KTEST_EXPECT_TRUE(result == forward + 2);
    KTEST_EXPECT_ALL(forward[2] == '0', forward[3] == '1', forward[4] == '2', forward[5] == '3');

    // memmove with a backward-overlapping region.
    char backward[] = {'0', '1', '2', '3', '4', '5'};
    result          = memmove(backward, backward + 2, 4);
    KTEST_EXPECT_TRUE(result == backward);
    KTEST_EXPECT_ALL(backward[0] == '2', backward[1] == '3', backward[2] == '4', backward[3] == '5');

    // Zero-length move leaves the buffer untouched but still returns dest.
    char zero[] = {'A', 'B', 'C'};
    result      = memmove(zero + 1, zero, 0);
    KTEST_EXPECT_TRUE(result == zero + 1);
    KTEST_EXPECT_ALL(zero[0] == 'A', zero[1] == 'B', zero[2] == 'C');

    // Full self-copy is a no-op.
    char self[] = {'X', 'Y', 'Z'};
    result      = memmove(self, self, sizeof(self));
    KTEST_EXPECT_TRUE(result == self);
    KTEST_EXPECT_ALL(self[0] == 'X', self[1] == 'Y', self[2] == 'Z');
}

KTEST_CASE(std_memset) {
    // Fill sets every byte and returns dest.
    char buffer[16];
    void* result = memset(buffer, 0xAB, sizeof(buffer));
    KTEST_EXPECT_TRUE(result == buffer);
    for (size_t i = 0; i < sizeof(buffer); ++i) { KTEST_EXPECT_EQUAL(static_cast<unsigned char>(buffer[i]), 0xAB); }

    // Zero-length memset leaves the buffer untouched.
    char untouched[]  = {'L', 'M', 'N'};
    size_t zero_bytes = 0;
    result            = memset(untouched, 0xCC, zero_bytes);
    KTEST_EXPECT_TRUE(result == untouched);
    KTEST_EXPECT_ALL(untouched[0] == 'L', untouched[1] == 'M', untouched[2] == 'N');
}

KTEST_CASE(std_memcmp) {
    const char lhs[]         = {'a', 'b', 'c', 'd'};
    const char rhs_equal[]   = {'a', 'b', 'c', 'd'};
    const char rhs_greater[] = {'a', 'b', 'c', 'e'};
    const char rhs_less[]    = {'a', 'b', 'b', 'd'};

    KTEST_EXPECT_EQUAL(memcmp(lhs, rhs_equal, sizeof(lhs)), 0);
    KTEST_EXPECT_TRUE(memcmp(lhs, rhs_greater, sizeof(lhs)) < 0);
    KTEST_EXPECT_TRUE(memcmp(lhs, rhs_less, sizeof(lhs)) > 0);

    // Zero length must not dereference either pointer.
    KTEST_EXPECT_EQUAL(memcmp(nullptr, nullptr, 0), 0);
}

KTEST_CASE(std_strlen) {
    KTEST_EXPECT_EQUAL(strlen(""), static_cast<size_t>(0));
    KTEST_EXPECT_EQUAL(strlen("archipelago"), static_cast<size_t>(11));
}
