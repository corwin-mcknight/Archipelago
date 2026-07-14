#pragma once

#include "kernel/interrupt.h"
#include "kernel/time.h"

namespace kernel::x86::drivers {

class pit_timer : public kernel::hal::IInterruptHandler {
   public:
    void init();
    time_ns_t resolution_ns();
    bool handle_interrupt(register_frame_t* regs);

   private:
    void set_phase(unsigned int hz);  //< Set the resolution
};

}  // namespace kernel::x86::drivers
