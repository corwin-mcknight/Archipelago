#include "kernel/cpu.h"

#include <kernel/boot.h>

#include <ktl/span>

#include "kernel/arch.h"
#include "kernel/config.h"
#include "kernel/log.h"
#include "kernel/mm/vm_aspace.h"
#include "kernel/platform.h"
#include "kernel/riscv/cpu.h"
#include "kernel/sched/scheduler.h"
#include "kernel/synchronization/execution_context.h"

namespace kernel::riscv {
void trap_init();  // riscv64/trap.cpp
}
namespace kernel::platform {
void timer_start_local();  // riscv64/timer.cpp
}

kernel::cpu_core g_cpu_cores[CONFIG_MAX_CORES];

size_t kernel::arch::current_core_index() { return kernel::riscv::current_core().index; }
void kernel::arch::set_kstack_floor(uintptr_t floor) { kernel::riscv::current_core().kstack_floor = floor; }
uintptr_t kernel::arch::kstack_floor() { return kernel::riscv::current_core().kstack_floor; }

namespace {

size_t started_core_count() {
    size_t count = kernel::boot::collect().cpu_count;
    return count > CONFIG_MAX_CORES ? CONFIG_MAX_CORES : count;
}

// Secondary-hart entry, released by cpu_start_cores() via kernel::boot::start_cpu(). Limine hands
// the hart the bootloader's page tables (reclaimable memory, so the kernel's own come first) and a
// fresh 64 KiB stack; every CSR is ours to set. Once the boot hart has the scheduler up, the hart
// joins it as its idle thread, arms its own tick, and runs whatever the shared queue offers.
// External interrupts stay with the boot hart (its PLIC context is the only one claimed).
[[noreturn]] void hart_entry(size_t core_index, uint64_t hartid) {
    auto& core  = g_cpu_cores[core_index];
    core.hartid = hartid;
    kernel::riscv::set_current_core(core);
    kernel::mm::kernel_aspace().activate();
    kernel::synchronization::init_execution_context(core_index);
    kernel::riscv::trap_init();
    uintptr_t sp;
    asm volatile("mv %0, sp" : "=r"(sp));
    kernel::arch::set_kstack_floor(sp - 48 * 1024);
    g_log.info("cpu{0} (hart {1}): up", core_index, hartid);
    core.initialized.store(true, ktl::memory_order::release);

    while (!kernel::sched::started()) { asm volatile("" ::: "memory"); }
    kernel::sched::join_secondary((uint32_t)core_index);
    kernel::platform::timer_start_local();
    kernel::arch::enable_interrupts();
    kernel::sched::idle_loop();
}

}  // namespace

void kernel::cpu_init_cores() {
    for (size_t i = 0; i < CONFIG_MAX_CORES; i++) {
        g_cpu_cores[i].index  = i;
        g_cpu_cores[i].hartid = UINT64_MAX;
        g_cpu_cores[i].initialized.store(false, ktl::memory_order::relaxed);
    }
}

void kernel::cpu_start_cores() {
    const auto& boot_info = kernel::boot::collect();
    if (boot_info.cpu_count > CONFIG_MAX_CORES) {
        g_log.warn("Firmware reported {0} harts but build supports only {1}; ignoring the rest", boot_info.cpu_count,
                   (size_t)CONFIG_MAX_CORES);
    }
    for (size_t i = 0; i < started_core_count(); i++) {
        if (i == boot_info.boot_cpu_index) { continue; }
        g_log.info("Starting cpu{0} (hart {1})", i, kernel::boot::cpu_hw_id(i));
        kernel::boot::start_cpu(i, hart_entry);
    }
}

// Bounded, unlike x86_64: a hart the bootloader lists but cannot start (the JH7110's S7 monitor hart
// has no supervisor mode) would otherwise hold the boot hart here until the watchdog fires.
void kernel::cpu_gate_wait_for_cores_started() {
    const uint64_t deadline = kernel::arch::timestamp() + kernel::platform::timestamp_hz();
    auto cores              = ktl::span(g_cpu_cores).first(started_core_count());
    for (;;) {
        bool all = true;
        for (auto& core : cores) { all = all && core.initialized.load(ktl::memory_order::acquire); }
        if (all) { return; }
        if (kernel::arch::timestamp() >= deadline) { break; }
    }
    for (auto& core : cores) {
        if (!core.initialized.load(ktl::memory_order::acquire)) {
            g_log.warn("cpu{0} (hart {1}) did not start; continuing without it", core.index,
                       kernel::boot::cpu_hw_id(core.index));
        }
    }
}
