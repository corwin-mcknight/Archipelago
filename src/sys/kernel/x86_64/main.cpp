#include <vendor/limine.h>

#include <ktl/circular_buffer>
#include <ktl/fixed_string>
#include <ktl/fmt>
#include <ktl/maybe>
#include <ktl/result>
#include <ktl/static_array>

#include "kernel/drivers/uart.h"
#include "kernel/log.h"
#include "kernel/panic.h"

extern "C" void init_global_constructors_array(void);

kernel::system_log g_log;
kernel::driver::uart uart;

void init_log() {
    uart.init();
    g_log.devices.push_back(&uart);
}

Result<bool, const char*> init_hardware() {
    auto res = Result<bool, const char*>{true};
    g_log.debug("Initializing Hardware...");

    // TODO:
    // * GDT, IDT & TSS, Paging, PIT...
    res = res.err("Failed to initialize GDT!");
    return res;
}

extern "C" void _start(void) {
    // Initialise runtime
    init_global_constructors_array();
    init_log();

    // Display version message
    g_log.info("Starting archipelago ver. {0}", "0.0.1");
    g_log.flush();

    auto did_init_hardware = init_hardware();
    if (did_init_hardware.is_err()) {
        g_log.fatal("Failed to initialize hardware: {0}", did_init_hardware.unwrap_err());
        panic("Was unable to finish initialization due to hardware initialization failure.");
    }
    panic("Hit end of kernel main!");
}