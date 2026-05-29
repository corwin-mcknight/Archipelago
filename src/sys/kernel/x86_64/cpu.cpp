#include "kernel/cpu.h"

#include <stddef.h>

#include "kernel/log.h"
#include "vendor/limine.h"

extern volatile struct limine_mp_request mp_request;
extern "C" void ap_startup(struct limine_mp_info* info);

void kernel::cpu_init_cores() {
    for (size_t i = 0; i < CONFIG_MAX_CORES; i++) {
        g_cpu_cores[i].initialized = false;
        g_cpu_cores[i].lapic_id    = 0xFFFFFFFF;
    }
}

void kernel::cpu_start_cores() {
    for (size_t i = 0; i < mp_request.response->cpu_count; i++) {
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
    while (true) {
        bool all_initialized = true;
        for (size_t i = 0; i < mp_request.response->cpu_count; i++) {
            if (!g_cpu_cores[i].initialized) {
                all_initialized = false;
                break;
            }
        }
        if (all_initialized) { break; }
    }
}