#include <kernel/cpu.h>
#include <kernel/x86/cpu.h>
#include <kernel/x86/descriptor_tables.h>
#include <string.h>

#include "kernel/config.h"

extern "C" void kernel_x86_install_gdt(uintptr_t);

// clang-format off
#define ISR_LIST(X) \
    X(0)  X(1)  X(2)  X(3)  X(4)  X(5)  X(6)  X(7)  X(8)  X(9)  \
    X(10) X(11) X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19) \
    X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) \
    X(30) X(31)
#define IRQ_LIST(X) \
    X(0)  X(1)  X(2)  X(3)  X(4)  X(5)  X(6)  X(7) \
    X(8)  X(9)  X(10) X(11) X(12) X(13) X(14) X(15) X(16)

#define DECLARE_ISR(n) extern "C" void interrupt_isr##n();
#define DECLARE_IRQ(n) extern "C" void interrupt_irq##n();
ISR_LIST(DECLARE_ISR)
IRQ_LIST(DECLARE_IRQ)
#undef DECLARE_ISR
#undef DECLARE_IRQ
// clang-format on

// Each CPU has it's own GDT.
kernel::x86::gdt gdts[CONFIG_MAX_CORES];

// They share the same IDT, which is initialized by the BP.
__attribute__((aligned(0x10))) struct kernel::x86::idt_entry idt[256];
__attribute__((aligned(0x10))) struct kernel::x86::idt_ptr idtptr;
bool idt_initialized = false;

void kernel::x86::init_gdt(int corenum) {
    gdts[corenum].entries[0]     = {0x0000, 0x00, 0x00, 0x00, 0x00, 0x00};  // Null Entry
    gdts[corenum].entries[1]     = {0xFFFF, 0x00, 0x00, 0x9A, 0xAF, 0x00};  // Kernel Code
    gdts[corenum].entries[2]     = {0xFFFF, 0x00, 0x00, 0x92, 0xAF, 0x00};  // Kernel Data
    gdts[corenum].entries[3]     = {0xFFFF, 0x00, 0x00, 0xF2, 0xAF, 0x00};  // User Data
    gdts[corenum].entries[4]     = {0xFFFF, 0x00, 0x00, 0xFA, 0xAF, 0x00};  // User Code

    // TSS
    uintptr_t tss_addr           = (uintptr_t)&gdts[corenum].tss;

    // Must stay at or past the segment limit: that is what tells the CPU there is no I/O permission
    // bitmap, so port access from CPL 3 faults. A base inside the limit -- zero included -- makes
    // the CPU read the TSS's own bytes as the bitmap and grant whatever they happen to spell.
    gdts[corenum].tss.iomap_base = sizeof(tss_entry);

    gdts[corenum].entries[5]     = {sizeof(tss_entry) - 1,
                                    (uint16_t)(tss_addr & 0xFFFF),
                                    (uint8_t)((tss_addr >> 16) & 0xFF),
                                    0xE9,
                                    0x00,
                                    (uint8_t)((tss_addr >> 24) & 0xFF)};  // Core TSS

    gdts[corenum].tss_entry.base = (tss_addr >> 32) & 0xFFFFFFFF;

    // The table spans the descriptor array plus the TSS descriptor's upper half. The TSS structure
    // sharing this struct is not part of it -- sizing the limit from sizeof(tss_entry) would let
    // selectors past the real end resolve to neighbouring fields as if they were descriptors.
    gdts[corenum].pointer.limit  = sizeof(gdts[corenum].entries) + sizeof(gdt_entry_high) - 1;
    gdts[corenum].pointer.base   = (uintptr_t)&gdts[corenum].entries;

    kernel_x86_install_gdt((uintptr_t)&gdts[corenum].pointer);
}

void kernel::x86::set_tss_rsp0(uintptr_t top) { gdts[kernel::x86::current_core_index()].tss.rsp[0] = top; }

void kernel::x86::idt_set_gate(unsigned char num, uintptr_t base, unsigned short sel, unsigned char flags) {
    idt[num].base_lo   = base & 0xFFFF;
    idt[num].base_mid  = (base >> 16) & 0xFFFF;
    idt[num].base_hi   = (base >> 32) & 0xFFFFFFFF;

    idt[num].selector  = sel;
    idt[num].flags     = flags;
    idt[num].ist       = 0;
    idt[num]._reserved = 0;
}

void kernel::x86::init_idt() {
    if (!idt_initialized) {
        idtptr.limit = sizeof(idt) - 1;
        idtptr.base  = (uintptr_t)&idt;

        memset(&idt, 0, sizeof(idt));

        // Install ISRs and IRQs via X-macro expansion
#define INSTALL_ISR(n) idt_set_gate(n, (uintptr_t)interrupt_isr##n, 0x08, 0x8E);
#define INSTALL_IRQ(n) idt_set_gate(32 + n, (uintptr_t)interrupt_irq##n, 0x08, 0x8E);
        ISR_LIST(INSTALL_ISR)
        IRQ_LIST(INSTALL_IRQ)
#undef INSTALL_ISR
#undef INSTALL_IRQ
        idt_initialized = true;
    }

    asm volatile("lidt %0" : : "m"(idtptr));
}
