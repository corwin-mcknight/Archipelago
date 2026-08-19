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

// No debug-exit device on real hardware; the nearest thing is SBI SRST
// (EID "SRST", FID 0), which asks OpenSBI to power the board off. The exit
// code has nowhere to go. Returns if the firmware lacks the extension, per
// the harness_exit contract.
void harness_exit(uint8_t) {
    register uint64_t a0 asm("a0") = 0;  // reset type: shutdown
    register uint64_t a1 asm("a1") = 0;  // reset reason: none
    register uint64_t a6 asm("a6") = 0;
    register uint64_t a7 asm("a7") = 0x53525354;
    asm volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a6), "r"(a7) : "memory");
}

uint64_t timestamp_hz() { return TIMEBASE_FREQ_HZ; }

// The timebase is a fixed board constant, so there is nothing to measure.
void timestamp_calibrate() {}

}  // namespace kernel::platform
