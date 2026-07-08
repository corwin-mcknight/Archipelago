#pragma once

#include <stdint.h>

#include "kernel/drivers/logging_device.h"

namespace kernel {
namespace driver {

// Per-arch access to the 16550's byte-wide register file (port I/O on x86_64,
// MMIO on riscv64); implemented in <arch>/uart.cpp.
bool uart_present();
uint8_t uart_reg_read(uint16_t offset);
void uart_reg_write(uint16_t offset, uint8_t value);

class uart : public logging_device {
    // Upper bound on transmit-ready polling in write_byte(). A real 16550 drains a byte in well
    // under a millisecond at any baud rate (a few thousand spins at most); one million iterations
    // is generously past that while keeping a wedged or absent port from hanging the kernel.
    constexpr static uint32_t TRANSMIT_SPIN_CAP = 1000000;

    bool m_healthy                              = false;

   public:
    const char* name() const override;
    void init() override;
    void write_byte(char c) override;
    bool transmit_empty();
    int received_data();
    char read();
    // True if the init loopback self-test passed and no transmit timeout has occurred since.
    bool healthy() const { return m_healthy; }
};

}  // namespace driver
}  // namespace kernel