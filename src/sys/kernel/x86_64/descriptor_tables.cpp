#include <kernel/cpu.h>
#include <kernel/x86/descriptor_tables.h>
#include <kernel/x86/ioport.h>
#include <string.h>

#include "kernel/config.h"

extern "C" void kernel_x86_install_gdt(uintptr_t);
extern "C" void interrupt_isr0();
extern "C" void interrupt_isr1();
extern "C" void interrupt_isr2();
extern "C" void interrupt_isr3();
extern "C" void interrupt_isr4();
extern "C" void interrupt_isr5();
extern "C" void interrupt_isr6();
extern "C" void interrupt_isr7();
extern "C" void interrupt_isr8();
extern "C" void interrupt_isr9();
extern "C" void interrupt_isr10();
extern "C" void interrupt_isr11();
extern "C" void interrupt_isr12();
extern "C" void interrupt_isr13();
extern "C" void interrupt_isr14();
extern "C" void interrupt_isr15();
extern "C" void interrupt_isr16();
extern "C" void interrupt_isr17();
extern "C" void interrupt_isr18();
extern "C" void interrupt_isr19();
extern "C" void interrupt_isr20();
extern "C" void interrupt_isr21();
extern "C" void interrupt_isr22();
extern "C" void interrupt_isr23();
extern "C" void interrupt_isr24();
extern "C" void interrupt_isr25();
extern "C" void interrupt_isr26();
extern "C" void interrupt_isr27();
extern "C" void interrupt_isr28();
extern "C" void interrupt_isr29();
extern "C" void interrupt_isr30();
extern "C" void interrupt_isr31();
extern "C" void interrupt_irq0();
extern "C" void interrupt_irq1();
extern "C" void interrupt_irq2();
extern "C" void interrupt_irq3();
extern "C" void interrupt_irq4();
extern "C" void interrupt_irq5();
extern "C" void interrupt_irq6();
extern "C" void interrupt_irq7();
extern "C" void interrupt_irq8();
extern "C" void interrupt_irq9();
extern "C" void interrupt_irq10();
extern "C" void interrupt_irq11();
extern "C" void interrupt_irq12();
extern "C" void interrupt_irq13();
extern "C" void interrupt_irq14();
extern "C" void interrupt_irq15();

// Each CPU has it's own GDT.
kernel::x86::gdt gdts[CONFIG_MAX_CORES];

// They share the same IDT, which is initialized by the BP.
__attribute__((aligned(0x10))) struct kernel::x86::idt_entry idt[256];
struct kernel::x86::idt_ptr idtptr;
bool idt_initialized = false;

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

void kernel::x86::disable_interrupts() { asm volatile("cli"); }
void kernel::x86::enable_interrupts() { asm volatile("sti"); }

void kernel::x86::idt_set_gate(unsigned char num, uintptr_t base, unsigned short sel, unsigned char flags) {
    idt[num].base_lo = base & 0xFFFF;
    idt[num].base_mid = (base >> 16) & 0xFFFF;
    idt[num].base_hi = (base >> 32) & 0xFFFFFFFF;

    idt[num].selector = sel;
    idt[num].flags = flags;
    idt[num].ist = 0;
    idt[num]._reserved = 0;
}

void kernel::x86::init_idt() {
    if (!idt_initialized) {
        idtptr.limit = sizeof(idt) - 1;
        idtptr.base = (uintptr_t)&idt;

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

        // Install the ISRs
        idt_set_gate(0, (uintptr_t)interrupt_isr0, 0x08, 0x8E);
        idt_set_gate(1, (uintptr_t)interrupt_isr1, 0x08, 0x8E);
        idt_set_gate(2, (uintptr_t)interrupt_isr2, 0x08, 0x8E);
        idt_set_gate(3, (uintptr_t)interrupt_isr3, 0x08, 0x8E);
        idt_set_gate(4, (uintptr_t)interrupt_isr4, 0x08, 0x8E);
        idt_set_gate(5, (uintptr_t)interrupt_isr5, 0x08, 0x8E);
        idt_set_gate(6, (uintptr_t)interrupt_isr6, 0x08, 0x8E);
        idt_set_gate(7, (uintptr_t)interrupt_isr7, 0x08, 0x8E);
        idt_set_gate(8, (uintptr_t)interrupt_isr8, 0x08, 0x8E);
        idt_set_gate(9, (uintptr_t)interrupt_isr9, 0x08, 0x8E);
        idt_set_gate(10, (uintptr_t)interrupt_isr10, 0x08, 0x8E);
        idt_set_gate(11, (uintptr_t)interrupt_isr11, 0x08, 0x8E);
        idt_set_gate(12, (uintptr_t)interrupt_isr12, 0x08, 0x8E);
        idt_set_gate(13, (uintptr_t)interrupt_isr13, 0x08, 0x8E);
        idt_set_gate(14, (uintptr_t)interrupt_isr14, 0x08, 0x8E);
        idt_set_gate(15, (uintptr_t)interrupt_isr15, 0x08, 0x8E);
        idt_set_gate(16, (uintptr_t)interrupt_isr16, 0x08, 0x8E);
        idt_set_gate(17, (uintptr_t)interrupt_isr17, 0x08, 0x8E);
        idt_set_gate(18, (uintptr_t)interrupt_isr18, 0x08, 0x8E);
        idt_set_gate(19, (uintptr_t)interrupt_isr19, 0x08, 0x8E);
        idt_set_gate(20, (uintptr_t)interrupt_isr20, 0x08, 0x8E);
        idt_set_gate(21, (uintptr_t)interrupt_isr21, 0x08, 0x8E);
        idt_set_gate(22, (uintptr_t)interrupt_isr22, 0x08, 0x8E);
        idt_set_gate(23, (uintptr_t)interrupt_isr23, 0x08, 0x8E);
        idt_set_gate(24, (uintptr_t)interrupt_isr24, 0x08, 0x8E);
        idt_set_gate(25, (uintptr_t)interrupt_isr25, 0x08, 0x8E);
        idt_set_gate(26, (uintptr_t)interrupt_isr26, 0x08, 0x8E);
        idt_set_gate(27, (uintptr_t)interrupt_isr27, 0x08, 0x8E);
        idt_set_gate(28, (uintptr_t)interrupt_isr28, 0x08, 0x8E);
        idt_set_gate(29, (uintptr_t)interrupt_isr29, 0x08, 0x8E);
        idt_set_gate(30, (uintptr_t)interrupt_isr30, 0x08, 0x8E);
        idt_set_gate(31, (uintptr_t)interrupt_isr31, 0x08, 0x8E);

        // IRQs
        idt_set_gate(32, (uintptr_t)interrupt_irq0, 0x08, 0x8E);
        idt_set_gate(33, (uintptr_t)interrupt_irq1, 0x08, 0x8E);
        idt_set_gate(34, (uintptr_t)interrupt_irq2, 0x08, 0x8E);
        idt_set_gate(35, (uintptr_t)interrupt_irq3, 0x08, 0x8E);
        idt_set_gate(36, (uintptr_t)interrupt_irq4, 0x08, 0x8E);
        idt_set_gate(37, (uintptr_t)interrupt_irq5, 0x08, 0x8E);
        idt_set_gate(38, (uintptr_t)interrupt_irq6, 0x08, 0x8E);
        idt_set_gate(39, (uintptr_t)interrupt_irq7, 0x08, 0x8E);
        idt_set_gate(40, (uintptr_t)interrupt_irq8, 0x08, 0x8E);
        idt_set_gate(41, (uintptr_t)interrupt_irq9, 0x08, 0x8E);
        idt_set_gate(42, (uintptr_t)interrupt_irq10, 0x08, 0x8E);
        idt_set_gate(43, (uintptr_t)interrupt_irq11, 0x08, 0x8E);
        idt_set_gate(44, (uintptr_t)interrupt_irq12, 0x08, 0x8E);
        idt_set_gate(45, (uintptr_t)interrupt_irq13, 0x08, 0x8E);
        idt_set_gate(46, (uintptr_t)interrupt_irq14, 0x08, 0x8E);
        idt_set_gate(47, (uintptr_t)interrupt_irq15, 0x08, 0x8E);
        idt_initialized = true;
    }

    asm volatile("lidt %0" : : "m"(idtptr));
}