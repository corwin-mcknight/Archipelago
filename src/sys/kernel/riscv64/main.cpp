#include <kernel/boot.h>

#include "kernel/arch.h"
#include "kernel/config.h"
#include "kernel/cpu.h"
#include "kernel/drivers/uart.h"
#include "kernel/interrupt.h"
#include "kernel/log.h"
#include "kernel/mm/early_heap.h"
#include "kernel/panic.h"
#include "kernel/riscv/timer.h"
#include "vendor/limine.h"

namespace kernel::riscv {
void trap_init();  // riscv64/trap.cpp
}

extern kernel::driver::uart uart;

kernel::riscv::drivers::sbi_timer timer;

extern "C" void init_global_constructors_array(void);

// Pin the paging mode: the paging code assumes a 4-level Sv48 walk and a
// mode-9 satp, so Sv39/Sv57 would silently corrupt every table access.
__attribute__((used, section(".limine_requests"))) volatile struct limine_paging_mode_request paging_mode_request = {
    .id       = LIMINE_PAGING_MODE_REQUEST,
    .revision = 0,
    .response = nullptr,
    .mode     = LIMINE_PAGING_MODE_RISCV_SV48,
    .max_mode = LIMINE_PAGING_MODE_RISCV_SV48,
    .min_mode = LIMINE_PAGING_MODE_RISCV_SV48};

extern uintptr_t _initial_heap_start;
extern uintptr_t _initial_heap_end;

extern "C" [[noreturn]] void _start(void) {
    g_early_heap.on_boot((uintptr_t)&_initial_heap_start, (uintptr_t)&_initial_heap_end);

    init_global_constructors_array();

    // The HHDM offset must be known before any MMIO device (including the
    // UART) is reachable, so it is resolved before the first log line. A
    // missing response leaves the UART unhealthy and the panic silent -- there
    // is no way to report anything without a mapped device.
    if (paging_mode_request.response == nullptr ||
        paging_mode_request.response->mode != LIMINE_PAGING_MODE_RISCV_SV48) {
        panic("Limine did not grant Sv48 paging");
    }
    kernel::boot::resolve_hhdm();

    uart.init();
    g_log.devices.push_back(&uart);

    kernel::cpu_init_cores();

    g_log.info("Starting Archipelago ver. {0} (riscv64)", CONFIG_KERNEL_VERSION);

    kernel::boot::snapshot_symbols();
    kernel::boot::init_memory();

    // Single-hart bring-up: install the trap vector, then let interrupts in.
    // CLINT/PLIC routing for external interrupts is future work.
    kernel::riscv::trap_init();
    g_interrupt_manager.initialize();
    kernel::arch::enable_interrupts();
    g_log.debug("cpu0: Interrupts Enabled");

    timer.init();

    kernel::cpu_start_cores();
    kernel::cpu_gate_wait_for_cores_started();

    g_log.info("riscv64: single-hart boot; secondary harts not started");

    kernel::boot::late_boot(0);
}
