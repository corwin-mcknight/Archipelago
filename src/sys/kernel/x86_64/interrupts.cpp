#include <kernel/interrupt.h>
#include <kernel/log.h>
#include <kernel/registers.h>
#include <kernel/x86/descriptor_tables.h>
#include <kernel/x86/ioport.h>

extern "C" void k_exception_handler(register_frame_t* regs) {
    g_log.error("Recieved exception {}", regs->int_no);
    g_log.error("Error code: {}", regs->err_code);

    // Page fault
    if (regs->int_no == 14) {
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        g_log.error("CR2: 0x{0:016p}", cr2);
    }

    // Send end of interrupt to PIC
    if (regs->int_no >= 40) { outb(0xA0, 0x20); }
    outb(0x20, 0x20);
}

extern "C" void k_irq_handler(register_frame_t* regs) {
    g_interrupt_manager.dispatch_interrupt((unsigned int)regs->int_no, regs);

    // Send end of interrupt to PIC
    if (regs->int_no >= 40) { outb(0xA0, 0x20); }
    outb(0x20, 0x20);
}