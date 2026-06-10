#include "kernel/interrupt.h"
#include "kernel/log.h"

kernel::hal::interrupt_manager g_interrupt_manager;

namespace kernel {
namespace hal {

void interrupt_manager::initialize() { memset(handlers, 0, sizeof(handlers)); }

// Register an object handler for interrupt id.
void interrupt_manager::register_interrupt(unsigned int id, IInterruptHandler* handler, uint64_t flags) {
    if (id >= IM_MAX_HANDLERS) { return; }
    handlers[id].handler.object = handler;
    handlers[id].flags = (flags | InterruptHandlerEntry::ENABLED_MASK) | InterruptHandlerEntry::OBJECT_HANDLER_MASK;
    g_log.trace("Registered interrupt 0x{0:x} with handler 0x{1:p}", id, (uint64_t)handler);
}

// Register a function handler for interrupt id.
void interrupt_manager::register_interrupt(unsigned int id, bool (*handler)(register_frame_t*), uint64_t flags) {
    if (id >= IM_MAX_HANDLERS) { return; }
    handlers[id].handler.function = handler;
    handlers[id].flags = (flags | InterruptHandlerEntry::ENABLED_MASK) & (~InterruptHandlerEntry::OBJECT_HANDLER_MASK);
    g_log.trace("Registered interrupt 0x{0:x} with handler 0x{1:p}", id, (uint64_t)handler);
}

void interrupt_manager::clear_handler(unsigned int id) {
    if (id >= IM_MAX_HANDLERS) { return; }
    handlers[id].handler.function = nullptr;
    handlers[id].flags            = 0;
}

void interrupt_manager::dispatch_interrupt(unsigned int id, register_frame_t* registers) {
    if (id >= IM_MAX_HANDLERS) { return; }
    if ((handlers[id].flags & InterruptHandlerEntry::ENABLED_MASK) == 0) {
        g_log.warn("Interrupt 0x{0:x} had no handlers listening for it.", id);
        return;
    }

    const bool ret = (handlers[id].flags & InterruptHandlerEntry::OBJECT_HANDLER_MASK)
                         ? handlers[id].handler.object->handle_interrupt(registers)  // Object handler
                         : handlers[id].handler.function(registers);                 // Function handler

    if (!ret) { g_log.error("Interrupt 0x{0:x} was not handled successfully", id); }
}

}  // namespace hal
}  // namespace kernel