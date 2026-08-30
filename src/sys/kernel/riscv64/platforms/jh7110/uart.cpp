#include <kernel/drivers/uart.h>
#include <kernel/mm/mmio.h>
#include <kernel/mm/physmap.h>

// The JH7110's UART0 is a DesignWare DW_apb_uart: a 16550 register file at
// physical 0x10000000, but with registers on a 4-byte stride accessed as
// 32-bit words (the DTS's reg-shift = 2, reg-io-width = 4). Reached through
// the HHDM; main.cpp publishes the direct map before calling init(). The driver
// logic lives in core/drivers/uart_16550.cpp.

namespace kernel::driver {

namespace {
constexpr uintptr_t UART0_PADDR = 0x10000000;
constinit kernel::mm::mmio_region g_uart_registers;
}  // namespace

bool uart_present() {
    if (!kernel::mm::direct_map_ready()) { return false; }
    if (!g_uart_registers.valid()) {
        g_uart_registers = kernel::mm::map_mmio({kernel::mm::physical_address(UART0_PADDR), 8 * sizeof(uint32_t)});
    }
    return true;
}

// Fences pair with the device's I/O ordering rules (the Linux readl/writel
// pattern): PMA strong ordering only covers accesses to the same address.
uint8_t uart_reg_read(uint16_t offset) {
    return static_cast<uint8_t>(g_uart_registers.read32(static_cast<size_t>(offset) << 2));
}

void uart_reg_write(uint16_t offset, uint8_t value) {
    g_uart_registers.write32(static_cast<size_t>(offset) << 2, value);
}

// 24 MHz UART input clock: 24e6 / (16 * 115200) rounds to 13 (0.2% fast).
uint16_t uart_divisor() { return 13; }

}  // namespace kernel::driver
