#pragma once

#include <stdint.h>

namespace kernel {
namespace x86 {
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
} __attribute__((packed));

void init_gdt(int corenum);

}  // namespace x86
};  // namespace kernel
