#include "kernel/cpu.h"

#include <kernel/boot.h>
#include <stddef.h>

#include <ktl/ranges>
#include <ktl/span>

#include "kernel/arch.h"
#include "kernel/config.h"
#include "kernel/log.h"
#include "kernel/x86/cpu.h"

kernel::cpu_core g_cpu_cores[CONFIG_MAX_CORES];

// Stack tripwire floor consumed by the interrupt stubs. One global: only the boot processor
// schedules on x86_64, and the APs park.
extern "C" uintptr_t g_kstack_floor = 0;

void kernel::arch::set_kstack_floor(uintptr_t floor) { g_kstack_floor = floor; }
uintptr_t kernel::arch::kstack_floor() { return g_kstack_floor; }

[[noreturn]] void ap_entry(size_t core_index, uint64_t hw_id);  // x86_64/main.cpp

size_t kernel::x86::current_core_index() {
    // CPUID leaf 1: the initial APIC id of the executing core is reported in EBX bits 31:24.
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1U), "c"(0U));
    const uint32_t lapic_id = ebx >> 24;

    // g_cpu_cores[] is keyed by the dense bootloader CPU-list position with the LAPIC id stored as
    // data (see core_init()); scan for the matching id to recover this core's dense index.
    for (auto [i, core] : ktl::views::enumerate(ktl::span(g_cpu_cores))) {
        if (core.lapic_id == lapic_id) { return i; }
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
