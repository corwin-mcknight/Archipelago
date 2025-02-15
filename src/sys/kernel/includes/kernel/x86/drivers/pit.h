#pragma once

#include "kernel/interrupt.h"
#include "kernel/time.h"

/// Represents a timer
class pit_timer : public kernel::hal::IInterruptHandler {
   public:
    void init();                //< Initialise
    time_ns_t resolution_ns();  //< Resolution in Nanoseconds
    bool handle_interrupt(register_frame_t* regs);

   private:
    void set_phase(unsigned int hz);  //< Set the resolution
};
