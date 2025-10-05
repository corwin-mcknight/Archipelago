#include <kernel/testing/testing.h>

#include <ktl/circular_buffer>
#include <ktl/fixed_string>
#include <ktl/fmt>
#include <ktl/maybe>
#include <ktl/result>
#include <ktl/static_array>

#include "kernel/assert.h"
#include "kernel/config.h"
#include "kernel/cpu.h"
#include "kernel/drivers/uart.h"
#include "kernel/interrupt.h"
#include "kernel/log.h"
#include "kernel/mm/early_heap.h"
#include "kernel/panic.h"
#include "kernel/x86/descriptor_tables.h"
#include "kernel/x86/drivers/pit.h"
#include "vendor/limine.h"

kernel::driver::uart uart;
kernel::x86::drivers::pit_timer timer;

extern "C" void init_global_constructors_array(void);

__attribute__((used, section(".limine_requests"))) volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST, .revision = 0, .response = nullptr, .flags = 0};

void core_init(uint32_t core_id) {
    assert(core_id < CONFIG_MAX_CORES, "Core ID exceeds maximum cores");
    g_cpu_cores[core_id].lapic_id = core_id;

    // Start basic hardware initialisation
    g_log.debug("cpu{0}: Initializing", core_id);
    kernel::x86::init_gdt((int)core_id);
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
    g_cpu_cores[core_id].initialized = true;
}

extern "C" [[noreturn]] void ap_startup(struct limine_mp_info* info) {
    g_log.info("cpu{0}: Starting", info->lapic_id);
    core_init(info->lapic_id);
    while (true) { __asm__ volatile("hlt"); }
}

extern uintptr_t _initial_heap_start;
extern uintptr_t _initial_heap_end;

extern "C" [[noreturn]] void _start(void) {
    g_early_heap.on_boot((uintptr_t)&_initial_heap_start, (uintptr_t)&_initial_heap_end);

    init_global_constructors_array();

    uart.init();
    g_log.devices.push_back(&uart);

    kernel::cpu_init_cores();

    g_log.info("Starting Archipelago ver. {0}", CONFIG_KERNEL_VERSION);

    if (mp_request.response == nullptr) { panic("Limine MP request failed"); }

    g_log.info("Booting on cpu{0}. CPU has {1} cores", mp_request.response->bsp_lapic_id,
               mp_request.response->cpu_count);

    core_init(mp_request.response->bsp_lapic_id);
    kernel::cpu_start_cores();
    kernel::cpu_gate_wait_for_cores_started();

#if CONFIG_KERNEL_TESTING
    g_log.info("Starting kernel tests...");
    kernel::testing::test_runner();
#else
    panic("Boot processor exited early");
#endif
}
