#include <kernel/log.h>
#include <kernel/registers.h>
#include <kernel/x86/descriptor_tables.h>

extern "C" void k_exception_handler(register_frame_t* regs) {
    g_log.info("Recieved exception {}", regs->int_no);
    g_log.info("Error code: {}", regs->err_code);
}

extern "C" void k_irq_handler(register_frame_t* regs) {
    g_log.info("Recieved IRQ {}", regs->int_no - kernel::x86::IRQ0);
}