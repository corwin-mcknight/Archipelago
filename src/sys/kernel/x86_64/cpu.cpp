#include "kernel/cpu.h"

#include <kernel/boot.h>
#include <stddef.h>

#include <ktl/span>

#include "kernel/arch.h"
#include "kernel/config.h"
#include "kernel/log.h"
#include "kernel/panic.h"
#include "kernel/x86/apic.h"
#include "kernel/x86/cpu.h"
#include "kernel/x86/descriptor_tables.h"

constinit kernel::cpu_core g_cpu_cores[CONFIG_MAX_CORES];

namespace {
kernel::x86::cpu_local g_cpu_locals[CONFIG_MAX_CORES];
constexpr uint32_t MSR_GS_BASE = 0xC0000101;
constinit ktl::atomic<uint64_t> g_reschedule_ipi_count{0};
}  // namespace

extern "C" void x86_note_reschedule_ipi();

void kernel::x86::install_local(size_t index) {
    if (index >= CONFIG_MAX_CORES) { panic("x86: CPU index out of range"); }
    g_cpu_locals[index]       = {};
    g_cpu_locals[index].index = index;
    wrmsr(MSR_GS_BASE, reinterpret_cast<uintptr_t>(&g_cpu_locals[index]));
}

kernel::x86::cpu_local& kernel::x86::local() {
    uintptr_t base = rdmsr(MSR_GS_BASE);
    if (base == 0) { panic("x86: GS local state is not installed"); }
    return *reinterpret_cast<kernel::x86::cpu_local*>(base);
}

uint64_t kernel::x86::reschedule_ipi_count() { return g_reschedule_ipi_count.load(ktl::memory_order::relaxed); }

extern "C" void x86_note_reschedule_ipi() { g_reschedule_ipi_count.fetch_add(1, ktl::memory_order::relaxed); }

void kernel::arch::set_kstack_floor(uintptr_t floor) { kernel::x86::local().kstack_floor = floor; }
void kernel::arch::send_reschedule_ipi(size_t core_index) {
    if (core_index >= CONFIG_MAX_CORES || !g_cpu_cores[core_index].initialized.load(ktl::memory_order::acquire)) {
        return;
    }
    kernel::x86::lapic_send_ipi(g_cpu_cores[core_index].lapic_id, kernel::x86::RESCHEDULE_IPI);
}
uintptr_t kernel::arch::kstack_floor() { return kernel::x86::local().kstack_floor; }

[[noreturn]] void ap_entry(size_t core_index, uint64_t hw_id);  // x86_64/main.cpp

size_t kernel::x86::current_core_index() {
    // Before core_init installs GS local state, early boot is single-core and CPUID is the only
    // per-CPU identity source available.  Once installed, GS avoids a CPUID on every scheduler
    // and interrupt-context lookup.
    uint64_t base = rdmsr(MSR_GS_BASE);
    if (base != 0) { return reinterpret_cast<kernel::x86::cpu_local*>(base)->index; }

    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1U), "c"(0U));
    uint32_t lapic_id = ebx >> 24;
    for (size_t i = 0; i < CONFIG_MAX_CORES; ++i) {
        if (g_cpu_cores[i].lapic_id == lapic_id) { return i; }
    }
    return 0;
}

size_t kernel::arch::current_core_index() { return kernel::x86::current_core_index(); }

void kernel::cpu_init_cores() {
    for (auto& core : ktl::span(g_cpu_cores)) {
        core.initialized.store(false, ktl::memory_order::relaxed);
        core.lapic_id = 0xFFFFFFFF;
    }
}

void kernel::cpu_start_cores() {
    const auto& boot_info = kernel::boot::collect();
    size_t core_count     = boot_info.cpu_count;
    if (core_count > CONFIG_MAX_CORES) {
        g_log.warn("Firmware reported {0} CPUs but build supports only {1}; ignoring the rest", core_count,
                   (size_t)CONFIG_MAX_CORES);
        core_count = CONFIG_MAX_CORES;
    }

    for (size_t i = 0; i < core_count; i++) {
        if (i == boot_info.boot_cpu_index) { continue; }
        g_log.info("Starting cpu{0} (lapic {1})", i, kernel::boot::cpu_hw_id(i));
        kernel::boot::start_cpu(i, ap_entry);
    }
}

void kernel::cpu_gate_wait_for_cores_started() {
    g_log.debug("Initializing other cores...");
    // Match the clamp in cpu_start_cores(): only cores we actually started can become initialized, and
    // g_cpu_cores has only CONFIG_MAX_CORES slots.
    size_t core_count = kernel::boot::collect().cpu_count;
    if (core_count > CONFIG_MAX_CORES) { core_count = CONFIG_MAX_CORES; }
    while (true) {
        bool all_initialized = true;
        for (auto& core : ktl::span(g_cpu_cores).first(core_count)) {
            if (!core.initialized.load(ktl::memory_order::acquire)) {
                all_initialized = false;
                break;
            }
        }
        if (all_initialized) { break; }
    }
}
