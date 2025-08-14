#pragma once

#include <stdint.h>

namespace kernel {
namespace x86 {

constexpr uint8_t IDT_ENTRY_ATTR_PRESENT = 0x80;
constexpr uint8_t IDT_ENTRY_ATTR_DPL0 = 0x00;
constexpr uint8_t IDT_ENTRY_ATTR_DPL3 = 0x60;
constexpr uint8_t IDT_ENTRY_ATTR_INTERRUPT = 0x05;

// IRQ
constexpr uint8_t IRQ0 = 32;
constexpr uint8_t IRQ1 = 33;
constexpr uint8_t IRQ2 = 34;
constexpr uint8_t IRQ3 = 35;
constexpr uint8_t IRQ4 = 36;
constexpr uint8_t IRQ5 = 37;
constexpr uint8_t IRQ6 = 38;
constexpr uint8_t IRQ7 = 39;
constexpr uint8_t IRQ8 = 40;
constexpr uint8_t IRQ9 = 41;
constexpr uint8_t IRQ10 = 42;
constexpr uint8_t IRQ11 = 43;
constexpr uint8_t IRQ12 = 44;
constexpr uint8_t IRQ13 = 45;
constexpr uint8_t IRQ14 = 46;
constexpr uint8_t IRQ15 = 47;

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));

struct tss_entry {
    uint32_t unused0;
    uint64_t rsp[3];
    uint64_t unused1;
    uint64_t ist[7];
    uint64_t unused2;
    uint16_t unused3;
    uint16_t iomap_base;
} __attribute__((packed));

struct gdt_entry_high {
    uint32_t base;
    uint32_t reserved0;
} __attribute__((packed));

struct gdt_pointer {
    uint16_t limit;
    uintptr_t base;
} __attribute__((packed));

struct gdt {
    struct gdt_entry entries[6];
    struct gdt_entry_high tss_entry;
    struct gdt_pointer pointer;
    struct tss_entry tss;
};

struct idt_entry {
    uint16_t base_lo;
    uint16_t selector;
    uint8_t ist;
    uint8_t flags;
    uint16_t base_mid;
    uint32_t base_hi;
    uint32_t _reserved;
} __attribute__((__packed__));

struct idt_ptr {
    uint16_t limit;
    uintptr_t base;
} __attribute__((__packed__));

void init_gdt(int corenum);
void init_idt();

void idt_set_gate(unsigned char num, uintptr_t base, unsigned short sel, unsigned char flags);
void enable_interrupts();
void disable_interrupts();

}  // namespace x86
};  // namespace kernel
