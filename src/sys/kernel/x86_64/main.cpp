#include <kernel/testing/testing.h>

#include <ktl/circular_buffer>
#include <ktl/fixed_string>
#include <ktl/fmt>
#include <ktl/maybe>
#include <ktl/result>
#include <ktl/static_array>

#include "kernel/assert.h"  // IWYU pragma: keep
#include "kernel/config.h"
#include "kernel/drivers/uart.h"
#include "kernel/interrupt.h"
#include "kernel/log.h"
#include "kernel/mm/page.h"
#include "kernel/panic.h"
#include "kernel/x86/cpu.h"
#include "kernel/x86/descriptor_tables.h"
#include "kernel/x86/drivers/pit.h"
#include "vendor/limine.h"  // IWYU pragma: keep
extern "C" void init_global_constructors_array(void);

kernel::driver::uart uart;
kernel::x86::drivers::pit_timer timer;
kernel::cpu_core cpu_cores[CONFIG_MAX_CORES];

__attribute__((used, section(".limine_requests_start"))) static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests"))) static volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST, .revision = 0, .response = nullptr, .flags = 0};

__attribute__((used, section(".limine_requests_end"))) static volatile LIMINE_REQUESTS_END_MARKER;

void init_log() {
    uart.init();
    g_log.devices.push_back(&uart);
}

void core_init(uint32_t core_id) {
    assert(core_id < CONFIG_MAX_CORES, "Core ID exceeds maximum cores");
    cpu_cores[core_id].lapic_id = core_id;

    // Start basic hardware initialisation
    g_log.debug("cpu{0}: Initializing", core_id);
    kernel::x86::init_gdt(core_id);
    kernel::x86::init_idt();

    if (core_id == 0) { g_interrupt_manager.initialize(); }

    kernel::x86::enable_interrupts();
    g_log.debug("cpu{0}: Interrupts Enabled", core_id);

    // Boot Processor is responsible for initializing the time
    if (core_id == 0) {
        timer.init();
        g_log.info("Time subsystem initialized", core_id);
    }

    g_log.debug("cpu{0}: Now running", core_id);
    cpu_cores[core_id].initialized = true;
}

extern "C" [[noreturn]] void ap_startup(struct limine_mp_info* info) {
    g_log.info("cpu{0}: Starting", info->lapic_id);
    core_init(info->lapic_id);
    while (true) { __asm__ volatile("hlt"); }
}

extern "C" [[noreturn]] void _start(void) {
    // Boot processor specific stuff...
    init_global_constructors_array();
    init_log();

    for (size_t i = 0; i < CONFIG_MAX_CORES; i++) {
        cpu_cores[i].initialized = false;
        cpu_cores[i].lapic_id    = 0xFFFFFFFF;
    }

    g_log.info("Starting Archipelago ver. {0}", CONFIG_KERNEL_VERSION);

    // Check the response...
    if (mp_request.response == nullptr) { panic("Limine MP request failed"); }

    g_log.info("Booted on cpu{0}. CPU has {1} cores", mp_request.response->bsp_lapic_id,
               mp_request.response->cpu_count);

    // Print more CPU info...
    for (size_t i = 0; i < mp_request.response->cpu_count; i++) {
        const struct limine_mp_info* cpu = mp_request.response->cpus[i];
        g_log.info("cpu{0}: APIC ID {1}, Processor ID {2}", i, cpu->lapic_id, cpu->processor_id);
    }

    core_init(mp_request.response->bsp_lapic_id);

    // Initalize other APs
    for (size_t i = 0; i < mp_request.response->cpu_count; i++) {
        const struct limine_mp_info* cpu = mp_request.response->cpus[i];
        if (cpu->lapic_id != mp_request.response->bsp_lapic_id) {
            g_log.info("Starting cpu{0}", cpu->lapic_id);
            __atomic_store_n(const_cast<void**>(reinterpret_cast<void* const*>(&cpu->goto_address)), (void*)ap_startup,
                             __ATOMIC_SEQ_CST);
        }
    }

    kernel::mm::vm_page test_page(0x1400, kernel::mm::vm_page_state::WIRED, 0b111);

#if CONFIG_KERNEL_TESTING
    g_log.info("Waiting for all cores to initialize...");
    // Wait for all cores to initialize by checking the initialized flag in cpu_cores
    while (true) {
        bool all_initialized = true;
        for (size_t i = 0; i < mp_request.response->cpu_count; i++) {
            if (!cpu_cores[i].initialized) {
                all_initialized = false;
                break;
            }
        }
        if (all_initialized) { break; }
    }

    g_log.info("All {0} cores initialized", mp_request.response->cpu_count);
    g_log.info("Starting kernel tests...");
    kernel::testing::test_runner();
#else
    // Kernel failed to start.
    panic("Boot processor exited early");
#endif
}
