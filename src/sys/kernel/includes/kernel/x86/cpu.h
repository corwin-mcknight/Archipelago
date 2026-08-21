#pragma once

#include <stddef.h>
#include <stdint.h>

#include <ktl/atomic>

#include "kernel/config.h"

namespace kernel {

struct cpu_core {
    ktl::atomic<bool> initialized;
    uint32_t lapic_id;
};

namespace x86 {

// Kept at fixed offsets because the interrupt and syscall entry assembly accesses this through
// IA32_GS_BASE.  User mode never changes GS today, so the kernel GS base remains live across
// SYSRET; install_local() establishes it before this CPU enables interrupts.
struct cpu_local {
    uintptr_t kstack_floor;
    uintptr_t syscall_kernel_rsp;
    uintptr_t syscall_user_rsp;
    size_t index;
};

static_assert(offsetof(cpu_local, kstack_floor) == 0);
static_assert(offsetof(cpu_local, syscall_kernel_rsp) == 8);
static_assert(offsetof(cpu_local, syscall_user_rsp) == 16);

/// Install this CPU's GS-based local state. Must precede any interrupt or syscall entry.
void install_local(size_t index);
cpu_local& local();
uint64_t reschedule_ipi_count();

/// @brief Dense logical index of the calling core.
/// The value is installed in this CPU's GS-based local state during bring-up.
size_t current_core_index();

/// Model-specific register access for the calling core.
inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo;
    uint32_t hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

inline void wrmsr(uint32_t msr, uint64_t value) {
    asm volatile("wrmsr" : : "c"(msr), "a"(static_cast<uint32_t>(value)), "d"(static_cast<uint32_t>(value >> 32)));
}

}  // namespace x86

namespace arch {

/// Program the SYSCALL/SYSRET MSRs on the calling core. x86_64-only; called from core_init().
void syscall_init();

}  // namespace arch

}  // namespace kernel

extern kernel::cpu_core g_cpu_cores[CONFIG_MAX_CORES];
