#include "kernel/cpu.h"

#include <kernel/boot.h>

#include <ktl/span>

#include "kernel/arch.h"
#include "kernel/config.h"
#include "kernel/interrupt.h"
#include "kernel/log.h"
#include "kernel/mm/vm_aspace.h"
#include "kernel/panic.h"
#include "kernel/platform.h"
#include "kernel/riscv/cpu.h"
#include "kernel/riscv/sbi.h"
#include "kernel/sched/scheduler.h"
#include "kernel/synchronization/execution_context.h"

namespace kernel::riscv {
void trap_init();  // riscv64/trap.cpp
}
constinit kernel::cpu_core g_cpu_cores[CONFIG_MAX_CORES];

size_t kernel::arch::current_core_index() { return kernel::riscv::current_core().index; }
void kernel::arch::set_kstack_floor(uintptr_t floor) { kernel::riscv::current_core().kstack_floor = floor; }
uintptr_t kernel::arch::kstack_floor() { return kernel::riscv::current_core().kstack_floor; }

namespace {
constexpr unsigned SUPERVISOR_SOFTWARE_CAUSE = 1;
constexpr uint64_t SIP_SSIP                  = 1ull << 1;
ktl::atomic<uint64_t> g_ipi_count{0};

// The reschedule IPI carries no payload: it asks the hart to revisit the run queue on its way
// out of the trap. Requesting it here rather than relying on the idle loop closes the window
// between idle finding the queue empty and entering wfi.
bool ipi_handler(register_frame_t*) {
    asm volatile("csrc sip, %0" ::"r"(SIP_SSIP));
    g_ipi_count.fetch_add(1, ktl::memory_order::relaxed);
    kernel::synchronization::request_preemption();
    return true;
}

// SBI hart masks are passed with base 0, so a hartid must fit one word.
uint64_t hart_bit(size_t core_index) {
    uint64_t hartid = kernel::boot::cpu_hw_id(core_index);
    if (hartid >= 64) { panic("sbi: hartid does not fit a hart mask"); }
    return 1ull << hartid;
}

void ipi_enable_local() { asm volatile("csrs sie, %0" ::"r"(SIP_SSIP)); }
}  // namespace

void kernel::riscv::ipi_init() {
    g_interrupt_manager.register_interrupt(SUPERVISOR_SOFTWARE_CAUSE, ipi_handler, 0);
    ipi_enable_local();
}

uint64_t kernel::riscv::ipi_count() { return g_ipi_count.load(ktl::memory_order::relaxed); }

void kernel::arch::send_reschedule_ipi(size_t core_index) {
    if (kernel::riscv::sbi::send_ipi(hart_bit(core_index)).error != 0) {
        g_log.warn("ipi: SBI send_ipi to cpu{0} failed", core_index);
    }
}

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
    ipi_enable_local();
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
