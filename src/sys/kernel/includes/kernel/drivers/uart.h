#pragma once

#include <stdint.h>

#include <ktl/string_view>

namespace kernel {
namespace driver {

// Per-board access to the 16550's register file (port I/O on x86_64, MMIO on
// riscv64); implemented in <arch>/platforms/<board>/uart.cpp.
bool uart_present();
uint8_t uart_reg_read(uint16_t offset);
void uart_reg_write(uint16_t offset, uint8_t value);
// Baud-rate divisor for 115200 on this board's UART input clock.
uint16_t uart_divisor();

class uart {
    // Upper bound on transmit-ready polling in write_byte(). A real 16550 drains a byte in well
    // under a millisecond at any baud rate (a few thousand spins at most); one million iterations
    // is generously past that while keeping a wedged or absent port from hanging the kernel.
    constexpr static uint32_t TRANSMIT_SPIN_CAP = 1000000;

    bool m_healthy                              = false;

    void write_raw(char c);

   public:
    void init();
    void write_byte(char c);
    void write_string(ktl::string_view s) {
        for (char c : s) { write_byte(c); }
    }
    bool transmit_empty();
    int received_data();
    char read();
};

}  // namespace driver
}  // namespace kernel
