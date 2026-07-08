#include <kernel/drivers/uart.h>

// Arch-neutral 16550 driver. The register file is identical on every port of
// the chip; only how a byte-wide register is reached differs (port I/O on
// x86_64, MMIO on riscv64), so that accessor lives in <arch>/uart.cpp.
using namespace kernel::driver;

const char* uart::name() const { return "uart"; }

void uart::init() {
    if (!uart_present()) { return; }         // bus unreachable; stay unhealthy so writes are dropped
    uart_reg_write(1, 0x00);                 // Disable all interrupts
    uart_reg_write(3, 0x80);                 // Enable DLAB (set baud rate divisor)
    uart_reg_write(0, 0x03);                 // Set divisor to 3 (lo byte) 38400 baud
    uart_reg_write(1, 0x00);                 //                  (hi byte)
    uart_reg_write(3, 0x03);                 // 8 bits, no parity, one stop bit
    uart_reg_write(2, 0xC7);                 // Enable FIFO, clear them, with 14-byte threshold
    uart_reg_write(4, 0x0B);                 // IRQs enabled, RTS/DSR set
    uart_reg_write(4, 0x1E);                 // Set in loopback mode, test the serial chip
    uart_reg_write(0, 0xAE);                 // Test serial chip (send byte 0xAE and check if serial returns same byte)
    m_healthy = (uart_reg_read(0) == 0xAE);  // a broken/absent port fails the echo -- mark it dead
    uart_reg_write(4, 0x0F);
}

void uart::write_byte(char c) {
    if (!m_healthy) { return; }  // never touch a dead or absent port
    // Bound the busy-wait so a wedged transmitter cannot hang the kernel (notably the panic path).
    for (uint32_t spins = 0; transmit_empty() == 0; spins++) {
        if (spins >= TRANSMIT_SPIN_CAP) {
            m_healthy = false;
            return;
        }
    }
    uart_reg_write(0, (uint8_t)c);
}

int uart::received_data() { return uart_reg_read(5) & 1; }
bool uart::transmit_empty() { return uart_reg_read(5) & 0x20; }

char uart::read() {
    while (received_data() == 0) {}
    return (char)uart_reg_read(0);
}
