#include <kernel/boot.h>
#include <kernel/config.h>
#include <kernel/cpu.h>
#include <kernel/testing/testing.h>
#include <kernel/x86/cpu.h>

#if CONFIG_KERNEL_TESTING

using namespace kernel::testing;

KTEST_MODULE("x86/cpu");

// Regression guard: the per-core tables (g_cpu_cores[], gdts[]) must be keyed on the dense
// logical core index -- the bootloader CPU-list position -- with the hardware LAPIC id stored as data
// rather than used as a subscript. After boot, every core in [0, min(cpu_count, CONFIG_MAX_CORES))
// must be initialized and hold the LAPIC id reported at that same list position.
//
// Note: QEMU hands out dense LAPIC ids 0..N-1, so sparse ids or ids >= CONFIG_MAX_CORES cannot be
// exercised here; the test locks the index<->lapic_id mapping and confirms every core completed
// bring-up. Run under `-smp >1` to exercise the AP startup path as well as the boot processor.
KTEST_CASE(cpu_smp_cores_initialized) {
    size_t count = kernel::boot::collect().cpu_count;
    KTEST_REQUIRE(count >= 1);
    if (count > CONFIG_MAX_CORES) { count = CONFIG_MAX_CORES; }

    for (size_t i = 0; i < count; i++) {
        KTEST_EXPECT_TRUE(g_cpu_cores[i].initialized.load(ktl::memory_order::acquire));
        KTEST_EXPECT_EQUAL((size_t)g_cpu_cores[i].lapic_id, (size_t)kernel::boot::cpu_hw_id(i));
    }
}

#endif
