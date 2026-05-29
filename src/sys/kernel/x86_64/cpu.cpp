#include "kernel/cpu.h"

#include <stddef.h>

#include "kernel/log.h"
#include "vendor/limine.h"

extern volatile struct limine_mp_request mp_request;
extern "C" void ap_startup(struct limine_mp_info* info);

void kernel::cpu_init_cores() {
    for (size_t i = 0; i < CONFIG_MAX_CORES; i++) {
        g_cpu_cores[i].initialized.store(false, ktl::memory_order::relaxed);
        g_cpu_cores[i].lapic_id = 0xFFFFFFFF;
    }
}

void kernel::cpu_start_cores() {
    // g_cpu_cores has only CONFIG_MAX_CORES slots; never start (and thus index) beyond that even if
    // firmware reports more CPUs than this build supports.
    size_t core_count = mp_request.response->cpu_count;
    if (core_count > CONFIG_MAX_CORES) {
        g_log.warn("Firmware reported {0} CPUs but build supports only {1}; ignoring the rest", core_count,
                   (size_t)CONFIG_MAX_CORES);
        core_count = CONFIG_MAX_CORES;
    }
    for (size_t i = 0; i < core_count; i++) {
        const struct limine_mp_info* cpu = mp_request.response->cpus[i];
        if (cpu->lapic_id != mp_request.response->bsp_lapic_id) {
            g_log.info("Starting cpu{0} (lapic {1})", i, cpu->lapic_id);
            // Publish this AP's dense logical index (its CPU-list position) before releasing it. The
            // SEQ_CST store of goto_address below acts as the release that makes extra_argument visible.
            const_cast<struct limine_mp_info*>(cpu)->extra_argument = i;
            __atomic_store_n(const_cast<void**>(reinterpret_cast<void* const*>(&cpu->goto_address)), (void*)ap_startup,
                             __ATOMIC_SEQ_CST);
        }
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
        for (size_t i = 0; i < core_count; i++) {
            if (!g_cpu_cores[i].initialized.load(ktl::memory_order::acquire)) {
                all_initialized = false;
                break;
            }
        }
        if (all_initialized) { break; }
    }
}