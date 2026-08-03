#pragma once
#include <stdint.h>

inline uint8_t inb(uint16_t port) {
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void outb(uint16_t port, uint8_t value) { asm volatile("outb %0, %1" : : "a"(value), "Nd"(port)); }

// Delay roughly one port-I/O transaction (~1 us) by writing to the POST diagnostic port. Devices
// slower than the CPU's port writes (the 8259 PIC) need this settling time between command writes.
inline void io_wait() { outb(0x80, 0); }
inline void outw(uint16_t port, uint16_t value) { asm volatile("outw %0, %1" : : "a"(value), "Nd"(port)); }