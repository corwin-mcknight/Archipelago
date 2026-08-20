#include <kernel/arch.h>
#include <kernel/drivers/uart.h>
#include <kernel/interrupt.h>
#include <kernel/platform.h>
#include <kernel/testing/testing.h>

KTEST_MODULE("riscv64/jh7110/uart");

namespace {

volatile uint32_t g_uart_interrupts = 0;

bool uart_thre_handler(register_frame_t*) {
    // Stop the level-triggered THRE condition before PLIC completion. Logging
    // here would retrigger the transmitter and is deliberately forbidden.
    kernel::driver::uart_reg_write(1, 0);
    __atomic_add_fetch(&g_uart_interrupts, 1u, __ATOMIC_RELAXED);
    return true;
}

}  // namespace

KTEST_CASE(jh7110_uart_thre_routes_through_plic) {
    unsigned int interrupt_id = kernel::platform::console_uart_interrupt_id();
    KTEST_REQUIRE_TRUE(interrupt_id > kernel::platform::BOARD_INTERRUPT_BASE);

    // Preserve console state even if firmware eventually leaves another UART
    // interrupt enabled. Archipelago currently initializes IER to zero.
    uint8_t saved_ier = kernel::driver::uart_reg_read(1);
    kernel::driver::uart_reg_write(1, 0);
    g_uart_interrupts = 0;
    g_interrupt_manager.register_interrupt(interrupt_id, uart_thre_handler, 0);

    // Wait until both THR and the shift register are empty, then enable only
    // THRE. This creates one interrupt without consuming serial RX traffic.
    uint64_t deadline = kernel::arch::timestamp() + kernel::platform::timestamp_hz() / 10;
    while ((kernel::driver::uart_reg_read(5) & 0x40) == 0 && kernel::arch::timestamp() < deadline) {}
    KTEST_REQUIRE_TRUE((kernel::driver::uart_reg_read(5) & 0x40) != 0);
    kernel::driver::uart_reg_write(1, 0x02);
    deadline = kernel::arch::timestamp() + kernel::platform::timestamp_hz() / 10;
    while (g_uart_interrupts == 0 && kernel::arch::timestamp() < deadline) {}

    g_interrupt_manager.clear_handler(interrupt_id);
    kernel::driver::uart_reg_write(1, saved_ier);
    KTEST_EXPECT_EQUAL(g_uart_interrupts, 1u);
}
