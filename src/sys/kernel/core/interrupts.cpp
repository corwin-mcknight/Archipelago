#include "kernel/interrupt.h"
#include "kernel/log.h"

kernel::hal::interrupt_manager g_interrupt_manager;

namespace kernel {
namespace hal {

void interrupt_manager::initialize() {
    for (unsigned int i = 0; i < IM_MAX_HANDLERS; i++) {
        handlers[i].handler.function = nullptr;
        handlers[i].flags = 0;
    }

    // Wipe out core stats
    for (int i = 0; i < CONFIG_MAX_CORES; i++) { core_reentrant_state[i] = 0; }
}

void interrupt_manager::register_interrupt(unsigned int id, IInterruptHandler* handler, uint64_t flags) {
    handlers[id].handler.object = handler;
    handlers[id].flags = flags;

    // Enable this interrupt
    handlers[id].flags |= InterruptHandlerEntry::ENABLED_MASK;
    // Set this as an object handler instead of a function handler
    handlers[id].flags |= 0b10;

    // Trace this
    g_log.trace("Registered interrupt 0x{0:x} with handler 0x{1:p}", id, (uint64_t)handler);
}

void interrupt_manager::register_interrupt(unsigned int id, bool (*handler)(register_frame_t*), uint64_t flags) {
    handlers[id].handler.function = handler;
    handlers[id].flags = flags;

    // Enable this interrupt
    handlers[id].flags |= InterruptHandlerEntry::ENABLED_MASK;

    // Set this as a function handler instead of an object handler
    handlers[id].flags &= ~(uint64_t)0b10;
}

void interrupt_manager::clear_handler(unsigned int id) {
    handlers[id].handler.function = nullptr;
    handlers[id].flags = 0;
}

void interrupt_manager::dispatch_interrupt(unsigned int id, register_frame_t* registers) {
    const int core = 0;  // For now.
    core_reentrant_state[core]++;

    // Ignore if it's 32, the timer interrupt
    if (id != 32) { g_log.trace("im: START int 0x{0:x} rep: {1}", id, core_reentrant_state[core] - 1); }

    // Check if the interrupt is enabled
    if ((handlers[id].flags & InterruptHandlerEntry::ENABLED_MASK) == 0) {
        g_log.warn("Interrupt 0x{0:x} is not enabled", id);
        return;
    }

    if ((handlers[id].flags & 0b10) == 0) {
        // Function handler
        if (!handlers[id].handler.function(registers)) {
            // If the handler returns false, we should log an error
            g_log.error("Interrupt 0x{0:x} was not handled successfully", id);
        }
    } else {
        // Object handler
        if (!handlers[id].handler.object->handle_interrupt(registers)) {
            g_log.error("Interrupt 0x{0:x} was not handled successfully", id);
        }
    }

    if (id != 32) { g_log.trace("Interrupt Manager: End {0}", id); }
    core_reentrant_state[core]--;
}

}  // namespace hal
}  // namespace kernel