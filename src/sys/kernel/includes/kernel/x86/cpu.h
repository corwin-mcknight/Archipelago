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

/// @brief Dense logical index of the calling core.
/// Reads the current LAPIC id via CPUID leaf 1 (EBX bits 31:24) and looks it up in g_cpu_cores[],
/// where core_init() stored each core's LAPIC id keyed by its dense bootloader CPU-list position.
/// Falls back to 0 if the id is not (yet) registered.
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