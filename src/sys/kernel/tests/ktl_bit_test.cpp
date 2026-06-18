#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <stddef.h>
#include <stdint.h>

#include <ktl/bit>

using namespace kernel::testing;

KTEST(ktl_bit_popcount, "ktl/bit") {
    KTEST_EXPECT_EQUAL(ktl::popcount(uint32_t{0}), 0);
    KTEST_EXPECT_EQUAL(ktl::popcount(uint32_t{0xFFFFFFFF}), 32);
    KTEST_EXPECT_EQUAL(ktl::popcount(uint8_t{0b10110010}), 4);
    static_assert(ktl::popcount(uint64_t{0xF}) == 4);
}

KTEST(ktl_bit_count_zeros, "ktl/bit") {
    // countl/countr on the full-width zero report the type width.
    KTEST_EXPECT_EQUAL(ktl::countl_zero(uint32_t{0}), 32);
    KTEST_EXPECT_EQUAL(ktl::countr_zero(uint32_t{0}), 32);

    KTEST_EXPECT_EQUAL(ktl::countl_zero(uint32_t{1}), 31);
    KTEST_EXPECT_EQUAL(ktl::countr_zero(uint32_t{1}), 0);

    // High and low bit positions are measured relative to the narrow type, not 64.
    KTEST_EXPECT_EQUAL(ktl::countl_zero(uint8_t{1}), 7);
    KTEST_EXPECT_EQUAL(ktl::countr_zero(uint32_t{0x80}), 7);
    KTEST_EXPECT_EQUAL(ktl::countl_zero(uint16_t{0x8000}), 0);
}

KTEST(ktl_bit_width, "ktl/bit") {
    KTEST_EXPECT_EQUAL(ktl::bit_width(uint32_t{0}), 0);
    KTEST_EXPECT_EQUAL(ktl::bit_width(uint32_t{1}), 1);
    KTEST_EXPECT_EQUAL(ktl::bit_width(uint32_t{0xFF}), 8);
    KTEST_EXPECT_EQUAL(ktl::bit_width(uint32_t{0x100}), 9);
}

KTEST(ktl_bit_single_bit, "ktl/bit") {
    KTEST_EXPECT_TRUE(ktl::has_single_bit(uint32_t{1}));
    KTEST_EXPECT_TRUE(ktl::has_single_bit(uint32_t{0x4000}));
    KTEST_EXPECT_FALSE(ktl::has_single_bit(uint32_t{0}));
    KTEST_EXPECT_FALSE(ktl::has_single_bit(uint32_t{3}));

    // is_power_of_two is the same predicate under the kernel's preferred name.
    KTEST_EXPECT_TRUE(ktl::is_power_of_two(size_t{4096}));
    KTEST_EXPECT_FALSE(ktl::is_power_of_two(size_t{4095}));
    static_assert(ktl::is_power_of_two(uint64_t{1} << 47));
}

KTEST(ktl_bit_floor_ceil, "ktl/bit") {
    KTEST_EXPECT_EQUAL((size_t)ktl::bit_floor(uint32_t{0}), (size_t)0);
    KTEST_EXPECT_EQUAL((size_t)ktl::bit_floor(uint32_t{1}), (size_t)1);
    KTEST_EXPECT_EQUAL((size_t)ktl::bit_floor(uint32_t{100}), (size_t)64);
    KTEST_EXPECT_EQUAL((size_t)ktl::bit_floor(uint32_t{128}), (size_t)128);

    KTEST_EXPECT_EQUAL((size_t)ktl::bit_ceil(uint32_t{0}), (size_t)1);
    KTEST_EXPECT_EQUAL((size_t)ktl::bit_ceil(uint32_t{1}), (size_t)1);
    KTEST_EXPECT_EQUAL((size_t)ktl::bit_ceil(uint32_t{100}), (size_t)128);
    KTEST_EXPECT_EQUAL((size_t)ktl::bit_ceil(uint32_t{128}), (size_t)128);
}

KTEST(ktl_bit_rotate, "ktl/bit") {
    KTEST_EXPECT_EQUAL(ktl::rotl(uint8_t{0b00010000}, 1), uint8_t{0b00100000});
    KTEST_EXPECT_EQUAL(ktl::rotl(uint8_t{0b10000000}, 1), uint8_t{0b00000001});
    KTEST_EXPECT_EQUAL(ktl::rotr(uint8_t{0b00000001}, 1), uint8_t{0b10000000});
    // Negative shift reverses direction; full-width rotate is identity.
    KTEST_EXPECT_EQUAL(ktl::rotl(uint8_t{0b00000001}, -1), uint8_t{0b10000000});
    KTEST_EXPECT_EQUAL((size_t)ktl::rotl(uint32_t{0xDEADBEEF}, 32), (size_t)0xDEADBEEF);
}

KTEST(ktl_bit_align, "ktl/bit") {
    KTEST_EXPECT_EQUAL(ktl::align_up(uintptr_t{0x1000}, 0x1000), uintptr_t{0x1000});
    KTEST_EXPECT_EQUAL(ktl::align_up(uintptr_t{0x1001}, 0x1000), uintptr_t{0x2000});
    KTEST_EXPECT_EQUAL(ktl::align_up(uintptr_t{0x1FFF}, 0x1000), uintptr_t{0x2000});
    KTEST_EXPECT_EQUAL(ktl::align_up(uintptr_t{0x1234}, 1), uintptr_t{0x1234});

    KTEST_EXPECT_EQUAL(ktl::align_down(uintptr_t{0x1FFF}, 0x1000), uintptr_t{0x1000});
    KTEST_EXPECT_EQUAL(ktl::align_down(uintptr_t{0x2000}, 0x1000), uintptr_t{0x2000});

    KTEST_EXPECT_TRUE(ktl::is_aligned(uintptr_t{0x2000}, 0x1000));
    KTEST_EXPECT_FALSE(ktl::is_aligned(uintptr_t{0x2001}, 0x1000));
    static_assert(ktl::align_up(uintptr_t{17}, 8) == 24);
}

#endif  // CONFIG_KERNEL_TESTING
