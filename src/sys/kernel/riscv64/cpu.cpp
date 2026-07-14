#include "kernel/cpu.h"

#include "kernel/arch.h"
#include "kernel/log.h"

// Single-hart bring-up; Limine MP (hartid-based) start lands with the SMP milestone.
void kernel::cpu_init_cores() {}

void kernel::cpu_start_cores() { g_log.info("riscv64: single-hart boot; secondary harts not started"); }

void kernel::cpu_gate_wait_for_cores_started() {}

size_t kernel::arch::current_core_index() { return 0; }
