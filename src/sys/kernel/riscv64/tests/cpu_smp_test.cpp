#include <kernel/arch.h>
#include <kernel/boot.h>
#include <kernel/config.h>
#include <kernel/riscv/cpu.h>
#include <kernel/testing/testing.h>

KTEST_MODULE("riscv64/cpu");

// Every hart in [0, min(cpu_count, CONFIG_MAX_CORES)) completed bring-up and holds the hartid reported
// at the same bootloader list position, and the boot hart resolves its own index through tp.
KTEST_CASE(riscv_smp_harts_initialized) {
    const auto& boot_info = kernel::boot::collect();
    size_t count          = boot_info.cpu_count;
    KTEST_REQUIRE(count >= 1);
    if (count > CONFIG_MAX_CORES) { count = CONFIG_MAX_CORES; }

    KTEST_EXPECT_EQUAL(kernel::arch::current_core_index(), boot_info.boot_cpu_index);
    for (size_t i = 0; i < count; i++) {
        KTEST_EXPECT_TRUE(g_cpu_cores[i].initialized.load(ktl::memory_order::acquire));
        KTEST_EXPECT_EQUAL(g_cpu_cores[i].hartid, kernel::boot::cpu_hw_id(i));
    }
}
