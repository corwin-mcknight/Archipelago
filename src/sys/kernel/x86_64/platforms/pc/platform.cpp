#include <kernel/arch.h>
#include <kernel/drivers/uart.h>
#include <kernel/log.h>
#include <kernel/platform.h>
#include <kernel/time.h>
#include <kernel/x86/ioport.h>
#include <kernel/x86/platforms/pc/pit.h>

extern kernel::driver::uart uart;

// Board facts for the PC: the legacy ISA device set that every x86_64 machine
// inherits. The counter itself (rdtsc) is a CPU register and stays in
// x86_64/arch.cpp; only its rate is established here, because that depends on
// the board's tick source.
namespace kernel::platform {

namespace {
uint64_t g_tsc_hz = 0;
// The 8254 PIT: the tick source every PC inherits. Interrupt-handler object, so
// its constructor must have run (global ctors) before timer_init().
pc::pit_timer g_timer;
}  // namespace

// The PC console is COM1, reached by port I/O, so nothing needs to be mapped first.
void console_init() {
    uart.init();
    g_log.devices.push_back(&uart);
}

void timer_init() {
    g_timer.init();
    g_log.info("Time subsystem initialized");
}

// QEMU's isa-debug-exit device, wired to port 0x604 by the test harness. The 0x2000 bias matches
// what the harness expects to decode from QEMU's exit status.
void harness_exit(uint8_t code) { outw(0x604, static_cast<uint16_t>(code | 0x2000)); }

uint64_t timestamp_hz() { return g_tsc_hz; }

void timestamp_calibrate() {
    // Measure TSC cycles across a run of kernel ticks. Wait for a tick edge first so the
    // window starts aligned; bound every wait so a dead timer degrades to hz=0 instead of
    // hanging boot (readers treat 0 as "print raw cycles").
    constexpr ktime_t CAL_TICKS = 10;
    // ~60-200x margin over the healthy ~10 ms window (~1e5-3e5 iterations). The pause loop body
    // is ~150 cycles/iteration on modern cores, so the worst-case cycle delta is ~3e9 -- safely
    // under the ~1.8e10 overflow bound of the * 1e9 multiply below -- and a dead-timer stall
    // stays well under a second.
    constexpr uint64_t SPIN_CAP = 20'000'000ull;
    ktime_t edge                = kernel::time::now() + 1;
    uint64_t spins              = 0;
    while (kernel::time::now() < edge && ++spins < SPIN_CAP) { asm volatile("pause"); }
    if (kernel::time::now() < edge) {
        g_log.warn("platform: timestamp calibration skipped (timer not ticking)");
        return;
    }
    uint64_t c0 = kernel::arch::timestamp();
    ktime_t t0  = kernel::time::now();
    spins       = 0;
    while (kernel::time::now() < t0 + CAL_TICKS && ++spins < SPIN_CAP) { asm volatile("pause"); }
    if (kernel::time::now() < t0 + CAL_TICKS) {
        g_log.warn("platform: timestamp calibration skipped (timer stalled mid-window)");
        return;
    }
    uint64_t c1  = kernel::arch::timestamp();
    time_ns_t ns = kernel::time::ktime_to_ns(kernel::time::now() - t0);
    if (ns == 0) {
        g_log.warn("platform: timestamp calibration skipped (zero-length window)");
        return;
    }
    g_tsc_hz = (c1 - c0) * 1'000'000'000ull / static_cast<uint64_t>(ns);
    g_log.info("platform: timestamp calibrated: {0} MHz", g_tsc_hz / 1'000'000);
}

}  // namespace kernel::platform
