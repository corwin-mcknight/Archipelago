#include <kernel/log.h>
#include <kernel/registers.h>
#include <kernel/time.h>
#include <kernel/x86/drivers/pit.h>
#include <kernel/x86/ioport.h>

#include <ktl/type_traits>

#include "kernel/interrupt.h"
#include "kernel/x86/descriptor_tables.h"

void pit_timer::init() {
    kernel::time::init(resolution_ns());
    g_interrupt_manager.register_interrupt(kernel::x86::IRQ0, this, 0);
    set_phase(1193);
}

bool pit_timer::handle_interrupt(register_frame_t* regs) {
    UNUSED(regs);

    kernel::time::tick();

    return true;
}

time_ns_t pit_timer::resolution_ns() { return 1000000000 / 1000; }

void pit_timer::set_phase(unsigned int divisor) {
    outb(0x43, 0x36);                       // Command 0x36
    outb(0x40, (uint8_t)(divisor & 0xFF));  // Send Divisor Lo
    outb(0x40, (uint8_t)(divisor >> 8));    // Send Divisor Hi
}