#include <kernel/drivers/uart.h>

// The JH7110's UART0 is a DesignWare DW_apb_uart: a 16550 register file at
// physical 0x10000000, but with registers on a 4-byte stride accessed as
// 32-bit words (the DTS's reg-shift = 2, reg-io-width = 4). Reached through
// the HHDM; main.cpp publishes the offset before calling init(). The driver
// logic lives in core/drivers/uart_16550.cpp.
extern uintptr_t g_hhdm_offset;

namespace kernel::driver {

namespace {
constexpr uintptr_t UART0_PADDR = 0x10000000;

volatile uint32_t* reg(uint16_t offset) {
    return reinterpret_cast<volatile uint32_t*>(g_hhdm_offset + UART0_PADDR + (uintptr_t{offset} << 2));
}
}  // namespace

bool uart_present() { return g_hhdm_offset != 0; }

// Fences pair with the device's I/O ordering rules (the Linux readl/writel
// pattern): PMA strong ordering only covers accesses to the same address.
uint8_t uart_reg_read(uint16_t offset) {
    uint32_t value = *reg(offset);
    asm volatile("fence i,r" ::: "memory");
    return static_cast<uint8_t>(value);
}

void uart_reg_write(uint16_t offset, uint8_t value) {
    asm volatile("fence w,o" ::: "memory");
    *reg(offset) = value;
}

// 24 MHz UART input clock: 24e6 / (16 * 115200) rounds to 13 (0.2% fast).
uint16_t uart_divisor() { return 13; }

}  // namespace kernel::driver
