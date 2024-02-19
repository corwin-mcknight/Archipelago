#include <kernel/drivers/uart.h>
#include <kernel/x86/ioport.h>

// Implement uart
using namespace kernel::driver;

const char* uart::name() const { return "uart"; }

void uart::init() {
    outb(port + 1, 0x00);  // Disable all interrupts
    outb(port + 3, 0x80);  // Enable DLAB (set baud rate divisor)
    outb(port + 0, 0x03);  // Set divisor to 3 (lo byte) 38400 baud
    outb(port + 1, 0x00);  //                  (hi byte)
    outb(port + 3, 0x03);  // 8 bits, no parity, one stop bit
    outb(port + 2, 0xC7);  // Enable FIFO, clear them, with 14-byte threshold
    outb(port + 4, 0x0B);  // IRQs enabled, RTS/DSR set
    outb(port + 4, 0x1E);  // Set in loopback mode, test the serial chip
    outb(port + 0, 0xAE);  // Test serial chip (send byte 0xAE and check if serial returns same byte)
    inb(port + 0);
    outb(port + 4, 0x0F);
}

void uart::write_byte(char c) {
    while (transmit_empty() == 0) {}
    outb(port, (uint8_t)c);
}

int uart::recieved_data() { return inb(port + 5) & 1; }
bool uart::transmit_empty() { return inb(port + 5) & 0x20; }

char uart::read() {
    while (recieved_data() == 0) {}
    return (char)inb(port);
}