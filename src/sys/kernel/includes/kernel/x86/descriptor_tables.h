#pragma once

#include <stdint.h>

namespace kernel {
namespace x86 {

// LAPIC timer tick vector (first vector past the CPU exceptions).
constexpr uint8_t IRQ0 = 32;

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
void set_tss_rsp0(uintptr_t top);

void idt_set_gate(unsigned char num, uintptr_t base, unsigned short sel, unsigned char flags);

}  // namespace x86
};  // namespace kernel
