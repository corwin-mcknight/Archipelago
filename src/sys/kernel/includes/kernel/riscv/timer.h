#pragma once

#include <kernel/interrupt.h>
#include <kernel/time.h>

namespace kernel::riscv::drivers {

/// Kernel tick timer backed by the SBI TIME extension and the supervisor timer interrupt.
class sbi_timer : public kernel::hal::IInterruptHandler {
   public:
    void init();
    time_ns_t resolution_ns();
    bool handle_interrupt(register_frame_t* regs);
};

}  // namespace kernel::riscv::drivers
