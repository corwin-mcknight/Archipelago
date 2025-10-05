#pragma once

#include "kernel/config.h"
#include "kernel/x86/cpu.h"

namespace kernel {
void cpu_init_cores();
void cpu_start_cores();
void cpu_gate_wait_for_cores_started();
}  // namespace kernel

extern kernel::cpu_core g_cpu_cores[CONFIG_MAX_CORES];