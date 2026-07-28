#include <kernel/drivers/uart.h>
#include <kernel/x86/ioport.h>

// 16550 register access for the legacy COM1 port; the driver logic lives in
// core/uart_16550.cpp.
namespace kernel::driver {

namespace { constexpr uint16_t UART0_PORT = 0x3f8; }

bool uart_present() { return true; }
uint8_t uart_reg_read(uint16_t offset) { return inb(UART0_PORT + offset); }
void uart_reg_write(uint16_t offset, uint8_t value) { outb(UART0_PORT + offset, value); }

}  // namespace kernel::driver
