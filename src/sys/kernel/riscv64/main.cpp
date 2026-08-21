#include <kernel/boot.h>

#include "kernel/arch.h"
#include "kernel/config.h"
#include "kernel/cpu.h"
#include "kernel/interrupt.h"
#include "kernel/log.h"
#include "kernel/mm/early_heap.h"
#include "kernel/panic.h"
#include "kernel/platform.h"
#include "kernel/riscv/cpu.h"
#include "kernel/synchronization/execution_context.h"

namespace kernel::riscv {
void trap_init();  // riscv64/trap.cpp
}

extern "C" void init_global_constructors_array(void);

extern uintptr_t _initial_heap_start;
extern uintptr_t _initial_heap_end;

// The trap entry compares sp against the hart's kstack floor, so it has to describe the stack in
// use before interrupts are enabled. Limine guarantees at least 64 KiB below the entry sp and this
// runs near its top, so 48 KiB down is legitimately reachable and the last 16 KiB is the tripwire.
static void arm_boot_stack_tripwire() {
    uintptr_t sp;
    asm volatile("mv %0, sp" : "=r"(sp));
    kernel::arch::set_kstack_floor(sp - 48 * 1024);
}

extern "C" [[noreturn]] void _start(void) {
    // tp must address a hart slot before anything consults the execution context; slot 0 stands in
    // until the boot hart's dense index is known.
    kernel::cpu_init_cores();
    kernel::riscv::set_current_core(g_cpu_cores[0]);

    g_early_heap.on_boot((uintptr_t)&_initial_heap_start, (uintptr_t)&_initial_heap_end);

    init_global_constructors_array();

    // The HHDM offset must be known before any MMIO device (including the
    // UART) is reachable, so it is resolved before the first log line. The
    // paging code assumes Sv39, so an ungranted mode is fatal -- and the panic
    // is silent, because there is no mapped device to report through.
    if (!kernel::boot::collect().paging_mode_ok) { panic("Boot protocol did not grant Sv39 paging"); }
    kernel::boot::resolve_hhdm();
    kernel::platform::watchdog_arm();

    kernel::platform::console_init();

    g_log.info("Starting Archipelago ver. {0} (riscv64)", CONFIG_KERNEL_VERSION);

    kernel::boot::snapshot_symbols();
    kernel::boot::init_memory();

    const auto& boot_info = kernel::boot::collect();
    if (boot_info.cpu_count == 0) { panic("Boot protocol reported no harts"); }
    if (boot_info.boot_cpu_index == SIZE_MAX) { panic("Boot hart not present in bootloader CPU list"); }
    if (boot_info.boot_cpu_index >= CONFIG_MAX_CORES) {
        panic("Boot hart sits past CONFIG_MAX_CORES in the bootloader CPU list; raise CONFIG_MAX_CORES");
    }
    const size_t bsp_index        = boot_info.boot_cpu_index;
    g_cpu_cores[bsp_index].hartid = kernel::boot::cpu_hw_id(bsp_index);
    kernel::riscv::set_current_core(g_cpu_cores[bsp_index]);
    kernel::synchronization::init_execution_context(bsp_index);
    g_log.info("Booting on hart {0}. {1} harts listed", g_cpu_cores[bsp_index].hartid, boot_info.cpu_count);

    // Install the trap vector (which also enables FP execution), then let interrupts in. The board
    // hook quiesces and configures its external-interrupt controller.
    kernel::riscv::trap_init();
    arm_boot_stack_tripwire();
    g_interrupt_manager.initialize();
    kernel::platform::interrupt_init();
    kernel::arch::enable_interrupts();
    g_log.debug("cpu{0}: Interrupts Enabled", bsp_index);

    kernel::platform::timer_init();

    g_cpu_cores[bsp_index].initialized.store(true, ktl::memory_order::release);
    kernel::cpu_start_cores();
    kernel::cpu_gate_wait_for_cores_started();

    kernel::boot::late_boot((uint32_t)bsp_index);
}
