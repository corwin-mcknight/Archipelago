#include <kernel/cpu.h>
#include <kernel/x86/descriptor_tables.h>
#include <kernel/x86/ioport.h>
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
    X(8)  X(9)  X(10) X(11) X(12) X(13) X(14) X(15)

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
    gdts[corenum].entries[3]     = {0xFFFF, 0x00, 0x00, 0xFA, 0xAF, 0x00};  // User Code
    gdts[corenum].entries[4]     = {0xFFFF, 0x00, 0x00, 0xF2, 0xAF, 0x00};  // User Data

    // TSS
    uintptr_t tss_addr           = (uintptr_t)&gdts[corenum].tss;

    gdts[corenum].entries[5]     = {sizeof(tss_entry),
                                    (uint16_t)(tss_addr & 0xFFFF),
                                    (uint8_t)((tss_addr >> 16) & 0xFF),
                                    0xE9,
                                    0x00,
                                    (uint8_t)((tss_addr >> 24) & 0xFF)};  // Core TSS

    gdts[corenum].tss_entry.base = (tss_addr >> 32) & 0xFFFFFFFF;

    gdts[corenum].pointer.limit  = sizeof(gdts[corenum].entries) + sizeof(tss_entry) - 1;
    gdts[corenum].pointer.base   = (uintptr_t)&gdts[corenum].entries;

    kernel_x86_install_gdt((uintptr_t)&gdts[corenum].pointer);
}

void kernel::x86::disable_interrupts() { asm volatile("cli"); }
void kernel::x86::enable_interrupts() { asm volatile("sti"); }

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

        // Clear the IDT
        memset(&idt, 0, sizeof(idt));

        // Remap the PIC
        outb(0x20, 0x11);
        outb(0xA0, 0x11);
        outb(0x21, 0x20);
        outb(0xA1, 0x28);
        outb(0x21, 0x04);
        outb(0xA1, 0x02);
        outb(0x21, 0x01);
        outb(0xA1, 0x01);
        outb(0x21, 0x00);
        outb(0xA1, 0x00);

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