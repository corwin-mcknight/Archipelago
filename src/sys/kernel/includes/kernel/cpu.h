#pragma once

// Arch-neutral CPU bring-up interface; each architecture implements these.
namespace kernel {
void cpu_init_cores();
void cpu_start_cores();
void cpu_gate_wait_for_cores_started();
}  // namespace kernel