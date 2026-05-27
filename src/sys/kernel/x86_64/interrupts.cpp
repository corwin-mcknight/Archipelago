#include <kernel/crash.h>
#include <kernel/interrupt.h>
#include <kernel/log.h>
#include <kernel/registers.h>
#include <kernel/x86/descriptor_tables.h>
#include <kernel/x86/ioport.h>

extern "C" void k_exception_handler(register_frame_t* regs) {
    // Vectors 0..31 are real CPU exceptions. The dispatcher never returns;
    // a recursion guard handles secondary faults during the dump itself.
    if (regs->int_no < 32) { kernel::crash::dispatch(kernel::crash::trigger_kind::exception, regs); }

    // Anything above 31 reaching this path is unexpected (the IRQ path uses
    // k_irq_handler). Log and try to continue so we don't lose visibility on
    // e.g. spurious hardware-injected vectors during early bring-up.
    g_log.error("k_exception_handler: unexpected vector {0} err=0x{1:x}", regs->int_no, regs->err_code);
    if (regs->int_no >= 40) { outb(0xA0, 0x20); }
    outb(0x20, 0x20);
}

extern "C" void k_irq_handler(register_frame_t* regs) {
    g_interrupt_manager.dispatch_interrupt((unsigned int)regs->int_no, regs);

    // Send end of interrupt to PIC
    if (regs->int_no >= 40) { outb(0xA0, 0x20); }
    outb(0x20, 0x20);
}