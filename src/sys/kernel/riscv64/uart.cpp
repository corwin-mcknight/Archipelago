#include <kernel/drivers/uart.h>

// QEMU virt's 16550 lives at physical 0x10000000 with byte-wide registers,
// reached through the HHDM (Limine maps the first 4 GiB there). main.cpp
// publishes the offset before calling init(). The driver logic lives in
// core/uart_16550.cpp.
extern uintptr_t g_hhdm_offset;

namespace kernel::driver {

namespace {
constexpr uintptr_t UART0_PADDR = 0x10000000;

volatile uint8_t* reg(uint16_t offset) {
    return reinterpret_cast<volatile uint8_t*>(g_hhdm_offset + UART0_PADDR + offset);
}
}  // namespace

bool uart_present() { return g_hhdm_offset != 0; }

// Fences pair with the device's I/O ordering rules (the Linux readb/writeb
// pattern): PMA strong ordering only covers accesses to the same address.
uint8_t uart_reg_read(uint16_t offset) {
    uint8_t value = *reg(offset);
    asm volatile("fence i,r" ::: "memory");
    return value;
}

void uart_reg_write(uint16_t offset, uint8_t value) {
    asm volatile("fence w,o" ::: "memory");
    *reg(offset) = value;
}

}  // namespace kernel::driver
