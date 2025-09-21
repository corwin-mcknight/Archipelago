#include <kernel/testing/testing.h>
#include <std/string.h>
#include <stddef.h>

using namespace kernel::testing;

KTEST(std_memcpy_basic, "std_string") {
    char source[] = {'A', 'B', 'C', 'D', 'E'};
    char destination[5] = {};

    void* result = memcpy(destination, source, sizeof(source));

    KTEST_REQUIRE_TRUE(result == destination);
    for (size_t i = 0; i < sizeof(source); ++i) { KTEST_EXPECT_EQUAL(destination[i], source[i]); }
}

KTEST(std_memset_fill, "std_string") {
    char buffer[16];

    void* result = memset(buffer, 0xAB, sizeof(buffer));

    KTEST_REQUIRE_TRUE(result == buffer);
    for (size_t i = 0; i < sizeof(buffer); ++i) {
        KTEST_EXPECT_EQUAL(static_cast<unsigned char>(buffer[i]), 0xAB);
    }
}

KTEST(std_memmove_overlap_forward, "std_string") {
    char buffer[] = {'0', '1', '2', '3', '4', '5'};

    void* result = memmove(buffer + 2, buffer, 4);

    KTEST_REQUIRE_TRUE(result == buffer + 2);
    KTEST_EXPECT_EQUAL(buffer[2], '0');
    KTEST_EXPECT_EQUAL(buffer[3], '1');
    KTEST_EXPECT_EQUAL(buffer[4], '2');
    KTEST_EXPECT_EQUAL(buffer[5], '3');
}

KTEST(std_memmove_overlap_backward, "std_string") {
    char buffer[] = {'0', '1', '2', '3', '4', '5'};

    void* result = memmove(buffer, buffer + 2, 4);

    KTEST_REQUIRE_TRUE(result == buffer);
    KTEST_EXPECT_EQUAL(buffer[0], '2');
    KTEST_EXPECT_EQUAL(buffer[1], '3');
    KTEST_EXPECT_EQUAL(buffer[2], '4');
    KTEST_EXPECT_EQUAL(buffer[3], '5');
}

KTEST(std_memcmp_variants, "std_string") {
    const char lhs[] = {'a', 'b', 'c', 'd'};
    const char rhs_equal[] = {'a', 'b', 'c', 'd'};
    const char rhs_greater[] = {'a', 'b', 'c', 'e'};
    const char rhs_less[] = {'a', 'b', 'b', 'd'};

    KTEST_EXPECT_EQUAL(memcmp(lhs, rhs_equal, sizeof(lhs)), 0);
    KTEST_EXPECT_TRUE(memcmp(lhs, rhs_greater, sizeof(lhs)) < 0);
    KTEST_EXPECT_TRUE(memcmp(lhs, rhs_less, sizeof(lhs)) > 0);
}

KTEST(std_strlen_basic, "std_string") {
    KTEST_EXPECT_EQUAL(strlen(""), static_cast<size_t>(0));
    KTEST_EXPECT_EQUAL(strlen("archipelago"), static_cast<size_t>(11));
}

KTEST(std_strcpy_basic, "std_string") {
    char destination[16];

    char* result = strcpy(destination, "kernel");

    KTEST_REQUIRE_TRUE(result == destination);
    KTEST_EXPECT_EQUAL(destination[0], 'k');
    KTEST_EXPECT_EQUAL(destination[5], 'l');
    KTEST_EXPECT_EQUAL(destination[6], '\0');
}

KTEST(std_strncpy_zero_pad, "std_string") {
    char destination[8];

    char* result = strncpy(destination, "os", sizeof(destination));

    KTEST_REQUIRE_TRUE(result == destination);
    KTEST_EXPECT_EQUAL(destination[0], 'o');
    KTEST_EXPECT_EQUAL(destination[1], 's');
    for (size_t i = 2; i < sizeof(destination); ++i) { KTEST_EXPECT_EQUAL(destination[i], '\0'); }
}

KTEST(std_strlcpy_truncation, "std_string") {
    char destination[5];

    size_t copied = strlcpy(destination, "abcdef", sizeof(destination));

    KTEST_REQUIRE_EQUAL(copied, static_cast<size_t>(6));
    KTEST_EXPECT_EQUAL(destination[0], 'a');
    KTEST_EXPECT_EQUAL(destination[3], 'd');
    KTEST_EXPECT_EQUAL(destination[4], '\0');
}

KTEST(std_strlcpy_no_truncation, "std_string") {
    char destination[16];

    size_t copied = strlcpy(destination, "arch", sizeof(destination));

    KTEST_REQUIRE_EQUAL(copied, static_cast<size_t>(4));
    KTEST_EXPECT_EQUAL(destination[0], 'a');
    KTEST_EXPECT_EQUAL(destination[3], 'h');
    KTEST_EXPECT_EQUAL(destination[4], '\0');
}

KTEST(std_strlcpy_zero_sized_destination, "std_string") {
    char guard = 'Z';

    size_t copied = strlcpy(&guard, "secure", 0);

    KTEST_REQUIRE_EQUAL(copied, static_cast<size_t>(6));
    KTEST_EXPECT_EQUAL(guard, 'Z');
}

KTEST(std_memmove_zero_length, "std_string") {
    char buffer[] = {'A', 'B', 'C'};

    void* result = memmove(buffer + 1, buffer, 0);

    KTEST_REQUIRE_TRUE(result == buffer + 1);
    KTEST_EXPECT_EQUAL(buffer[0], 'A');
    KTEST_EXPECT_EQUAL(buffer[1], 'B');
    KTEST_EXPECT_EQUAL(buffer[2], 'C');
}

KTEST(std_memmove_self_copy, "std_string") {
    char buffer[] = {'X', 'Y', 'Z'};

    void* result = memmove(buffer, buffer, sizeof(buffer));

    KTEST_REQUIRE_TRUE(result == buffer);
    KTEST_EXPECT_EQUAL(buffer[0], 'X');
    KTEST_EXPECT_EQUAL(buffer[1], 'Y');
    KTEST_EXPECT_EQUAL(buffer[2], 'Z');
}

KTEST(std_memset_zero_length, "std_string") {
    char buffer[] = {'L', 'M', 'N'};

    size_t zero_bytes = 0;
    void* result = memset(buffer, 0xCC, zero_bytes);

    KTEST_REQUIRE_TRUE(result == buffer);
    KTEST_EXPECT_EQUAL(buffer[0], 'L');
    KTEST_EXPECT_EQUAL(buffer[1], 'M');
    KTEST_EXPECT_EQUAL(buffer[2], 'N');
}

KTEST(std_memcmp_zero_length_nullptr, "std_string") { KTEST_EXPECT_EQUAL(memcmp(nullptr, nullptr, 0), 0); }
