#include <kernel/drivers/uart.h>
#include <kernel/log.h>
#include <kernel/platform.h>

extern kernel::driver::uart uart;

// Board facts for the StarFive JH7110 (Orange Pi RV, VisionFive 2). The
// counter itself (rdtime) is a CPU register and lives in riscv64/arch.cpp;
// its rate is the board's timebase, so it lives here.
namespace kernel::platform {

namespace {
// The JH7110's mtime timebase, per its DTS /cpus/timebase-frequency. Reading
// the DTB instead of hardcoding per board is tracked as a todo.
constexpr uint64_t TIMEBASE_FREQ_HZ = 4'000'000;

}  // namespace

// UART0 is a DW_apb_uart reached through the HHDM, so resolve_hhdm() must have
// run before this.
void console_init() { uart.init(); }

// No fixed interrupt hardware needs quiescing; PLIC routing is future work.
void interrupt_init() {}

// No debug-exit device on real hardware, and SBI SRST dead-ends in OpenSBI's
// pm-reset (the PMIC sits on an I2C bus whose clocks U-Boot gates off at EFI
// handoff). The exit code has nowhere to go, but the contract's intent --
// this kernel instance is done, the harness restarts it -- is a reboot here,
// which also spares crash-expected board tests the 60s watchdog wait.
void harness_exit(uint8_t) { reboot(); }

uint64_t timestamp_hz() { return TIMEBASE_FREQ_HZ; }

// The timebase is a fixed board constant, so there is nothing to measure.
void timestamp_calibrate() {}

}  // namespace kernel::platform
