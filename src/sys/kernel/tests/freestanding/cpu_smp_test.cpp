#include <kernel/config.h>
#include <kernel/cpu.h>
#include <kernel/testing/testing.h>
#include <vendor/limine.h>

#if CONFIG_KERNEL_TESTING

extern volatile struct limine_mp_request mp_request;

using namespace kernel::testing;

// Regression guard for F005: the per-core tables (g_cpu_cores[], gdts[]) must be keyed on the dense
// logical core index -- the bootloader CPU-list position -- with the hardware LAPIC id stored as data
// rather than used as a subscript. After boot, every core in [0, min(cpu_count, CONFIG_MAX_CORES))
// must be initialized and hold the LAPIC id reported at that same list position.
//
// Note: QEMU hands out dense LAPIC ids 0..N-1, so this cannot reproduce the sparse/>=16-id boot panic
// that F005 describes; it locks the index<->lapic_id mapping and confirms every core completed
// bring-up. Run under `-smp >1` to exercise the AP startup path as well as the boot processor.
KTEST(cpu_smp_cores_initialized, "x86/cpu") {
    KTEST_REQUIRE(mp_request.response != nullptr);

    uint64_t count = mp_request.response->cpu_count;
    KTEST_REQUIRE(count >= 1);
    if (count > CONFIG_MAX_CORES) { count = CONFIG_MAX_CORES; }

    for (uint64_t i = 0; i < count; i++) {
        KTEST_EXPECT_TRUE(g_cpu_cores[i].initialized.load(ktl::memory_order::acquire));
        KTEST_EXPECT_EQUAL((size_t)g_cpu_cores[i].lapic_id, (size_t)mp_request.response->cpus[i]->lapic_id);
    }
}

#endif
