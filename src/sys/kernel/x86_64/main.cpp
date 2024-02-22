#include <vendor/limine.h>

#include <ktl/circular_buffer>
#include <ktl/fixed_string>
#include <ktl/fmt>
#include <ktl/maybe>
#include <ktl/result>
#include <ktl/static_array>

#include "kernel/config.h"
#include "kernel/drivers/uart.h"
#include "kernel/log.h"
#include "kernel/panic.h"
#include "kernel/x86/descriptor_tables.h"

extern "C" void init_global_constructors_array(void);

kernel::system_log g_log;
kernel::driver::uart uart;

void init_log() {
    uart.init();
    g_log.devices.push_back(&uart);
}

void core_init(int core_id) {
    // Start basic hardware initialisation
    g_log.debug("<core{0}> state=init", core_id);
    kernel::x86::init_gdt(core_id);
    // kernel::x86::init_idt(core_id);

    // Boot Processor is responsible for initializing the time
    if (core_id == 0) {
        kernel::time::init(104500000);  // TODO: Replace Dummy Number with real timer resolution
        g_log.info("Time subsystem initialized", core_id);
    }

    g_log.debug("<core{0}> state=running", core_id);
}

void bp_init() {
    init_log();
    // Display version message
    g_log.info("Starting Archipelago ver. {0}", CONFIG_KERNEL_VERSION);

    // Init timing

    core_init(0);
}

extern "C" void _start(void) {
    // Initialise runtime
    init_global_constructors_array();
    bp_init();

    // Kernel failed to start.
    g_log.fatal(
        "The system was unable to boot successfully, and will now halt completely.\n"
        "The system will be entirely unresponsive, and will need to be restarted.");
    panic("Boot processor exited early");
    // Processor is halted.
}
