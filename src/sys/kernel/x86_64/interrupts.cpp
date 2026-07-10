#include <kernel/crash.h>
#include <kernel/interrupt.h>
#include <kernel/log.h>
#include <kernel/registers.h>
#include <kernel/x86/descriptor_tables.h>
#include <kernel/x86/ioport.h>

extern "C" bool x86_try_resolve_page_fault(register_frame_t* regs);

extern "C" void k_exception_handler(register_frame_t* regs) {
    // Page faults get one shot at demand-paging resolution before the crash
    // path; an unresolvable fault falls through with diagnostics intact.
    if (regs->int_no == 14 && x86_try_resolve_page_fault(regs)) { return; }

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
    // EOI first: the tick handler may switch threads and not return here for a while. PIC lines
    // are edge-triggered, so early EOI cannot re-raise a level.
    if (regs->int_no >= 40) { outb(0xA0, 0x20); }
    outb(0x20, 0x20);

    g_interrupt_manager.dispatch_interrupt((unsigned int)regs->int_no, regs);
}