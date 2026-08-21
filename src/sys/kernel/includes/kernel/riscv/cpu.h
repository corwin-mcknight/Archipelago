#pragma once

#include <stddef.h>
#include <stdint.h>

#include <ktl/atomic>

#include "kernel/config.h"

namespace kernel {

// Per-hart state. The calling hart's entry is addressed by tp in supervisor mode and by sscratch
// while user code runs; the trap entry reads the first three fields by fixed byte offset
// (kstack_top at 0, user_sp at 8, kstack_floor at 16), so they must stay first and in this order.
struct cpu_core {
    uintptr_t kstack_top;    // stack the next trap from user mode lands on
    uintptr_t user_sp;       // parking slot for the interrupted user sp during trap entry
    uintptr_t kstack_floor;  // stack-overflow tripwire for the thread running on this hart
    size_t index;            // dense bootloader CPU-list position; the subscript into g_cpu_cores
    uint64_t hartid;
    ktl::atomic<bool> initialized;
};

namespace riscv {

inline cpu_core& current_core() {
    cpu_core* core;
    asm volatile("mv %0, tp" : "=r"(core));
    return *core;
}

// Point tp at this hart's entry; must run before anything consults the execution context.
inline void set_current_core(cpu_core& core) { asm volatile("mv tp, %0" ::"r"(&core)); }

}  // namespace riscv
}  // namespace kernel

extern kernel::cpu_core g_cpu_cores[CONFIG_MAX_CORES];
