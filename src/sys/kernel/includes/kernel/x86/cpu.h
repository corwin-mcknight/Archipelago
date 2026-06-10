#pragma once

#include <stddef.h>
#include <stdint.h>

#include <ktl/atomic>

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

}  // namespace x86

}  // namespace kernel