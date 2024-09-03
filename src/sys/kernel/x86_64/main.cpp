#include <vendor/limine.h>

#include <ktl/circular_buffer>
#include <ktl/fixed_string>
#include <ktl/fmt>
#include <ktl/maybe>
#include <ktl/result>
#include <ktl/static_array>

#include "kernel/assert.h"
#include "kernel/config.h"
#include "kernel/drivers/uart.h"
#include "kernel/log.h"
#include "kernel/panic.h"
#include "kernel/x86/descriptor_tables.h"
#include "vendor/limine.h"

extern "C" void init_global_constructors_array(void);

kernel::system_log g_log;
kernel::driver::uart uart;

static volatile struct limine_rsdp_request rsdp_request = {.id = LIMINE_RSDP_REQUEST, .revision = 0};

void init_log() {
    uart.init();
    g_log.devices.push_back(&uart);
}

void init_acpi() {
    // Print RDSP info
    if (rsdp_request.response == NULL) { g_log.error("No RSDP found, halting"); }

    auto rsdp = rsdp_request.response;
    g_log.info("RSDP: 0x{0:p}", (uint64_t)rsdp->address);
}

void init_core_tables(int core_id) {
    kernel::x86::init_gdt(core_id);
    kernel::x86::init_idt();
    kernel::x86::enable_interrupts();
    g_log.debug("cpu{0}: Interrupts Enabled", core_id);
}
void core_init(int core_id) {
    // Start basic hardware initialisation
    g_log.debug("cpu{0}: Initializing", core_id);
    init_core_tables(core_id);

    // Boot Processor is responsible for initializing the time
    if (core_id == 0) {
        kernel::time::init(104500000);  // TODO: Replace Dummy Number with real timer resolution
        g_log.info("Time subsystem initialized", core_id);

        init_acpi();
    }

    assert(core_id == 0, "Does not support booting other cores");

    g_log.debug("cpu{0}: Now running", core_id);
}

extern "C" void _start(void) {
    // Boot processor specific stuff...
    init_global_constructors_array();
    init_log();
    g_log.info("Starting Archipelago ver. {0}", CONFIG_KERNEL_VERSION);
    core_init(0);

    // Kernel failed to start.
    panic("Boot processor exited early");
    // Processor is halted.
}
