#include <kernel/testing/expect.h>
#include <kernel/testing/testing.h>
#include <stddef.h>
#include <stdint.h>

#include <ktl/bit>

using namespace kernel::testing;

KTEST_MODULE("ktl/bit");

KTEST_CASE(ktl_bit_power_of_two_queries) {
    EXPECT(ktl::is_power_of_two(uint32_t{1}));
    EXPECT(ktl::is_power_of_two(uint32_t{0x4000}));
    EXPECT(!ktl::is_power_of_two(uint32_t{0}));
    EXPECT(!ktl::is_power_of_two(uint32_t{3}));
    EXPECT(ktl::is_power_of_two(size_t{4096}));
    EXPECT(!ktl::is_power_of_two(size_t{4095}));
    static_assert(ktl::is_power_of_two(uint64_t{1} << 47));
}

KTEST_CASE(ktl_bit_align) {
    EXPECT(ktl::align_up(uintptr_t{0x1000}, 0x1000) == uintptr_t{0x1000});
    EXPECT(ktl::align_up(uintptr_t{0x1001}, 0x1000) == uintptr_t{0x2000});
    EXPECT(ktl::align_up(uintptr_t{0x1FFF}, 0x1000) == uintptr_t{0x2000});
    EXPECT(ktl::align_up(uintptr_t{0x1234}, 1) == uintptr_t{0x1234});

    EXPECT(ktl::align_down(uintptr_t{0x1FFF}, 0x1000) == uintptr_t{0x1000});
    EXPECT(ktl::align_down(uintptr_t{0x2000}, 0x1000) == uintptr_t{0x2000});
    static_assert(ktl::align_up(uintptr_t{17}, 8) == 24);
    static_assert(ktl::is_unsigned_v<decltype(ktl::align_up(uintptr_t{17}, 8))>);
}
