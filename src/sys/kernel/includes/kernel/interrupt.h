#pragma once

#include <stdint.h>

#include "kernel/config.h"

typedef struct register_frame register_frame_t;

#define IM_MAX_HANDLERS 256

namespace kernel {
namespace hal {

/// @brief The IInterruptHandler class is an interface for handling interrupts.
class IInterruptHandler {
   public:
    virtual bool handle_interrupt(register_frame_t* regs) = 0;
};

struct InterruptHandlerEntry {
    union Handler {
        bool (*function)(register_frame_t*);
        IInterruptHandler* object;
        Handler() : function(nullptr) {}
    } handler;

    // Flags store information about this interrupt.
    constexpr static uint64_t ENABLED_MASK = 0b01;
    const static uint64_t OBJECT_HANDLER = 0b10;
    uint64_t flags;
};

class interrupt_manager {
   public:
    void initialize();

    void register_interrupt(unsigned int id, IInterruptHandler* handler, uint64_t flags);
    void register_interrupt(unsigned int id, bool (*handler)(register_frame_t*), uint64_t flags);
    void clear_handler(unsigned int id);
    void dispatch_interrupt(unsigned int id, register_frame_t* registers);

   private:
    InterruptHandlerEntry handlers[IM_MAX_HANDLERS];
    int core_reentrant_state[CONFIG_MAX_CORES];

    [[maybe_unused]] int pad;  // purposefully unused
};
}  // namespace hal
}  // namespace kernel

extern kernel::hal::interrupt_manager g_interrupt_manager;