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
#include "kernel/panic.h"
#include "kernel/x86/descriptor_tables.h"
#include "kernel/x86/drivers/pit.h"
#include "vendor/limine.h"  // IWYU pragma: keep

extern "C" void init_global_constructors_array(void);

kernel::driver::uart uart;
kernel::x86::drivers::pit_timer timer;

void init_log() {
    uart.init();
    g_log.devices.push_back(&uart);
}

void core_init(int core_id) {
    assert(core_id == 0, "Does not support booting other cores");
    // Start basic hardware initialisation
    g_log.debug("cpu{0}: Initializing", core_id);
    kernel::x86::init_gdt(core_id);
    kernel::x86::init_idt();

    if (core_id == 0) { g_interrupt_manager.initialize(); }

    kernel::x86::enable_interrupts();
    g_log.debug("cpu{0}: Interrupts Enabled", core_id);

    // Boot Processor is responsible for initializing the time
    if (core_id == 0) {
        // Set up the timer...
        timer.init();
        g_log.info("Time subsystem initialized", core_id);
    }

    g_log.debug("cpu{0}: Now running", core_id);
}

extern "C" [[noreturn]] void _start(void) {
    // Boot processor specific stuff...
    init_global_constructors_array();
    init_log();

    g_log.info("Starting Archipelago ver. {0}", CONFIG_KERNEL_VERSION);
    core_init(0);

#if CONFIG_KERNEL_TESTING
    kernel::testing::test_runner();
#else
    // Kernel failed to start.
    panic("Boot processor exited early");
#endif
}
