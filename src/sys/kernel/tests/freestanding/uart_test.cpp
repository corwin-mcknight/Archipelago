#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/drivers/uart.h>

extern kernel::driver::uart uart;

using namespace kernel::testing;

// F039: the boot-time loopback self-test must leave the console UART marked healthy. QEMU's
// emulated 16550 always passes loopback, so this verifies init() records the echo result instead
// of discarding it. The dead-port paths (failed loopback, transmit timeout) are not exercisable
// under QEMU.
KTEST(uart_console_healthy_after_boot, "kernel/uart") { KTEST_REQUIRE_TRUE(uart.healthy()); }

#endif
