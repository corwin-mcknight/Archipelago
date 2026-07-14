#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING
#include <kernel/arch.h>
#include <kernel/interrupt.h>

KTEST_MODULE("riscv64/arch");

KTEST_CASE(riscv_translation_root_active) {
    // Sv48 is live from boot; the active root must be a real page-aligned frame.
    KTEST_REQUIRE_TRUE(kernel::arch::active_translation_root() != 0);
    KTEST_EXPECT_EQUAL(kernel::arch::active_translation_root() & 0xFFFull, 0ull);
}

KTEST_CASE(riscv_interrupt_save_restore) {
    uint64_t saved = kernel::arch::save_and_disable_interrupts();
    KTEST_EXPECT_FALSE(kernel::arch::interrupts_enabled());
    kernel::arch::restore_interrupts(saved);
    KTEST_EXPECT_EQUAL(kernel::arch::interrupts_enabled(), (saved & 0x2) != 0);
}

namespace {
volatile bool g_ssi_fired = false;

bool ssi_handler(register_frame_t*) {
    // Clear the pending bit from inside the handler or the trap re-fires
    // forever on sret.
    asm volatile("csrc sip, %0" ::"r"(1ull << 1));
    g_ssi_fired = true;
    return true;
}
}  // namespace

// Round-trip the asynchronous trap path: pend a supervisor software interrupt
// (cause code 1) against ourselves and require the trap entry, dispatcher,
// and handler chain to deliver it.
KTEST_CASE(riscv_software_interrupt_round_trip) {
    constexpr unsigned SSI_CODE = 1;
    g_ssi_fired                 = false;
    g_interrupt_manager.register_interrupt(SSI_CODE, ssi_handler, 0);

    asm volatile("csrs sie, %0" ::"r"(1ull << 1));  // enable SSIE
    asm volatile("csrs sip, %0" ::"r"(1ull << 1));  // pend it against this hart

    // Delivery is effectively immediate once pending with interrupts on;
    // spin generously so a slow TCG host cannot flake the test.
    for (int i = 0; i < 1000000 && !g_ssi_fired; ++i) { asm volatile("nop"); }

    asm volatile("csrc sie, %0" ::"r"(1ull << 1));
    g_interrupt_manager.clear_handler(SSI_CODE);
    KTEST_EXPECT_TRUE(g_ssi_fired);
}
#endif
