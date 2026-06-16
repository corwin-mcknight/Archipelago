#include "kernel/cpu.h"

#include <stddef.h>

#include <ktl/ranges>
#include <ktl/span>

#include "kernel/config.h"
#include "kernel/log.h"
#include "vendor/limine.h"

extern volatile struct limine_mp_request mp_request;
extern "C" void ap_startup(struct limine_mp_info* info);

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

void kernel::cpu_init_cores() {
    for (auto& core : ktl::span(g_cpu_cores)) {
        core.initialized.store(false, ktl::memory_order::relaxed);
        core.lapic_id = 0xFFFFFFFF;
    }
}

void kernel::cpu_start_cores() {
    auto* response    = mp_request.response;
    size_t core_count = response->cpu_count;
    if (core_count > CONFIG_MAX_CORES) {
        g_log.warn("Firmware reported {0} CPUs but build supports only {1}; ignoring the rest", core_count,
                   (size_t)CONFIG_MAX_CORES);
        core_count = CONFIG_MAX_CORES;
    }

    const uint32_t bsp = response->bsp_lapic_id;
    auto cpus          = ktl::span(response->cpus, core_count);
    auto not_bsp       = [bsp](const auto& p) { return p.second->lapic_id != bsp; };

    for (auto [i, cpu] : cpus | ktl::views::enumerate | ktl::views::filter(not_bsp)) {
        g_log.info("Starting cpu{0} (lapic {1})", i, cpu->lapic_id);
        // Publish this AP's dense logical index (its CPU-list position) before releasing it. The
        // SEQ_CST store of goto_address below acts as the release that makes extra_argument visible.
        const_cast<struct limine_mp_info*>(cpu)->extra_argument = i;
        __atomic_store_n(const_cast<void**>(reinterpret_cast<void* const*>(&cpu->goto_address)), (void*)ap_startup,
                         __ATOMIC_SEQ_CST);
    }
}

void kernel::cpu_gate_wait_for_cores_started() {
    g_log.debug("Initializing other cores...");
    // Match the clamp in cpu_start_cores(): only cores we actually started can become initialized, and
    // g_cpu_cores has only CONFIG_MAX_CORES slots.
    size_t core_count = mp_request.response->cpu_count;
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
