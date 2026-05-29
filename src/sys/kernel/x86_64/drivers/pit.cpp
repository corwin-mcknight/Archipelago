#include <kernel/log.h>
#include <kernel/registers.h>
#include <kernel/time.h>
#include <kernel/x86/drivers/pit.h>
#include <kernel/x86/ioport.h>

#include <ktl/type_traits>

#include "kernel/interrupt.h"
#include "kernel/x86/descriptor_tables.h"

namespace kernel::x86::drivers {

// PIT base frequency in Hz and the divisor programmed into the channel-0 reload register.
// These are the single source of truth for both set_phase() and resolution_ns().
static constexpr uint64_t PIT_BASE_FREQ_HZ = 1193182;
static constexpr unsigned int PIT_DIVISOR  = 1193;

void pit_timer::init() {
    kernel::time::init(resolution_ns());
    g_interrupt_manager.register_interrupt(kernel::x86::IRQ0, this, 0);
    set_phase(PIT_DIVISOR);
}

bool pit_timer::handle_interrupt(register_frame_t* regs) {
    UNUSED(regs);

    kernel::time::tick();

    return true;
}

time_ns_t pit_timer::resolution_ns() { return (time_ns_t)((uint64_t)PIT_DIVISOR * 1000000000ULL / PIT_BASE_FREQ_HZ); }

void pit_timer::set_phase(unsigned int divisor) {
    outb(0x43, 0x36);                       // Command 0x36
    outb(0x40, (uint8_t)(divisor & 0xFF));  // Send Divisor Lo
    outb(0x40, (uint8_t)(divisor >> 8));    // Send Divisor Hi
}

}  // namespace kernel::x86::drivers
