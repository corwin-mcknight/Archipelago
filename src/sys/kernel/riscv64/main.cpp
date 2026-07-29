#include <kernel/boot.h>

#include "kernel/arch.h"
#include "kernel/config.h"
#include "kernel/cpu.h"
#include "kernel/interrupt.h"
#include "kernel/log.h"
#include "kernel/mm/early_heap.h"
#include "kernel/panic.h"
#include "kernel/platform.h"
#include "kernel/synchronization/execution_context.h"

namespace kernel::riscv {
void trap_init();  // riscv64/trap.cpp
}

extern "C" void init_global_constructors_array(void);

extern uintptr_t _initial_heap_start;
extern uintptr_t _initial_heap_end;

extern "C" [[noreturn]] void _start(void) {
    g_early_heap.on_boot((uintptr_t)&_initial_heap_start, (uintptr_t)&_initial_heap_end);

    init_global_constructors_array();

    // The HHDM offset must be known before any MMIO device (including the
    // UART) is reachable, so it is resolved before the first log line. The
    // paging code assumes Sv48, so an ungranted mode is fatal -- and the panic
    // is silent, because there is no mapped device to report through.
    if (!kernel::boot::collect().paging_mode_ok) { panic("Boot protocol did not grant Sv48 paging"); }
    kernel::boot::resolve_hhdm();

    kernel::platform::console_init();

    kernel::cpu_init_cores();

    g_log.info("Starting Archipelago ver. {0} (riscv64)", CONFIG_KERNEL_VERSION);

    kernel::boot::snapshot_symbols();
    kernel::boot::init_memory();

    kernel::synchronization::init_execution_context(0);

    // Single-hart bring-up: install the trap vector, then let interrupts in.
    // CLINT/PLIC routing for external interrupts is future work.
    kernel::riscv::trap_init();
    g_interrupt_manager.initialize();
    kernel::arch::enable_interrupts();
    g_log.debug("cpu0: Interrupts Enabled");

    kernel::platform::timer_init();

    kernel::cpu_start_cores();
    kernel::cpu_gate_wait_for_cores_started();

    g_log.info("riscv64: single-hart boot; secondary harts not started");

    kernel::boot::late_boot(0);
}
