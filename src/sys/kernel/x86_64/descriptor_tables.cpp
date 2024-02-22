#include <kernel/cpu.h>
#include <kernel/x86/descriptor_tables.h>

#include "kernel/config.h"

// gdts
kernel::x86::gdt gdts[CONFIG_MAX_CORES];

extern "C" void kernel_x86_install_gdt(uintptr_t);

void kernel::x86::init_gdt(int corenum) {
    gdts[corenum].entries[0] = {0x0000, 0x00, 0x00, 0x00, 0x00, 0x00};  // Null Entry
    gdts[corenum].entries[1] = {0xFFFF, 0x00, 0x00, 0x9A, 0xAF, 0x00};  // Kernel Code
    gdts[corenum].entries[2] = {0xFFFF, 0x00, 0x00, 0x92, 0xAF, 0x00};  // Kernel Data
    gdts[corenum].entries[3] = {0xFFFF, 0x00, 0x00, 0xFA, 0xAF, 0x00};  // User Code
    gdts[corenum].entries[4] = {0xFFFF, 0x00, 0x00, 0xF2, 0xAF, 0x00};  // User Data

    // TSS
    uintptr_t tss_addr = (uintptr_t)&gdts[corenum].tss;

    gdts[corenum].entries[5] = {sizeof(tss_entry),
                                (uint16_t)(tss_addr & 0xFFFF),
                                (uint8_t)((tss_addr >> 16) & 0xFF),
                                0xE9,
                                0x00,
                                (uint8_t)((tss_addr >> 24) & 0xFF)};  // Core TSS

    gdts[corenum].tss_entry.base = (tss_addr >> 32) & 0xFFFFFFFF;

    gdts[corenum].pointer.limit = sizeof(gdts[corenum].entries) + sizeof(tss_entry) - 1;
    gdts[corenum].pointer.base = (uintptr_t)&gdts[corenum].entries;

    kernel_x86_install_gdt((uintptr_t)&gdts[corenum].pointer);
}