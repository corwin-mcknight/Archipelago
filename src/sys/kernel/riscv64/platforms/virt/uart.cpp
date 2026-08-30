#include <kernel/drivers/uart.h>
#include <kernel/mm/mmio.h>
#include <kernel/mm/physmap.h>

// QEMU virt's 16550 lives at physical 0x10000000 with byte-wide registers,
// reached through the HHDM (Limine maps the first 4 GiB there). main.cpp
// publishes the direct map before calling init(). The driver logic lives in
// core/drivers/uart_16550.cpp.

namespace kernel::driver {

namespace {
constexpr uintptr_t UART0_PADDR = 0x10000000;
constinit kernel::mm::mmio_region g_uart_registers;
}  // namespace

bool uart_present() {
    if (!kernel::mm::direct_map_ready()) { return false; }
    if (!g_uart_registers.valid()) {
        g_uart_registers = kernel::mm::map_mmio({kernel::mm::physical_address(UART0_PADDR), 8});
    }
    return true;
}

// Fences pair with the device's I/O ordering rules (the Linux readb/writeb
// pattern): PMA strong ordering only covers accesses to the same address.
uint8_t uart_reg_read(uint16_t offset) { return g_uart_registers.read8(offset); }

void uart_reg_write(uint16_t offset, uint8_t value) { g_uart_registers.write8(offset, value); }

// QEMU models the standard 1.8432 MHz reference clock: divisor 1 is 115200.
uint16_t uart_divisor() { return 1; }

}  // namespace kernel::driver
