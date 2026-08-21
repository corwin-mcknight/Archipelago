#include <kernel/arch.h>
#include <kernel/interrupt.h>
#include <kernel/riscv/cpu.h>
#include <kernel/testing/testing.h>

KTEST_MODULE("riscv64/arch");

KTEST_CASE(riscv_translation_root_active) {
    // Sv39 is live from boot; the active root must be a real page-aligned frame.
    KTEST_REQUIRE_TRUE(kernel::arch::active_translation_root() != 0);
    KTEST_EXPECT_EQUAL(kernel::arch::active_translation_root() & 0xFFFull, 0ull);
}

KTEST_CASE(riscv_interrupt_save_restore) {
    uint64_t saved = kernel::arch::save_and_disable_interrupts();
    KTEST_EXPECT_FALSE(kernel::arch::interrupts_enabled());
    kernel::arch::restore_interrupts(saved);
    KTEST_EXPECT_EQUAL(kernel::arch::interrupts_enabled(), (saved & 0x2) != 0);
}

// Round-trip the reschedule IPI through SBI and the asynchronous trap path: send one to the
// calling hart and require the trap entry, dispatcher, and the kernel's handler to take it.
KTEST_CASE(riscv_reschedule_ipi_round_trip) {
    uint64_t before = kernel::riscv::ipi_count();
    kernel::arch::send_reschedule_ipi(kernel::arch::current_core_index());
    // Delivery is effectively immediate once pending with interrupts on; spin generously so a
    // slow TCG host cannot flake the test.
    for (int i = 0; i < 1000000 && kernel::riscv::ipi_count() == before; ++i) { asm volatile("nop"); }
    KTEST_EXPECT_TRUE(kernel::riscv::ipi_count() > before);
}
