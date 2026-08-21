#include <kernel/arch.h>
#include <kernel/assert.h>
#include <kernel/drivers/uart.h>
#include <kernel/interrupt.h>
#include <kernel/log.h>
#include <kernel/platform.h>
#include <kernel/time.h>
#include <kernel/x86/apic.h>
#include <kernel/x86/descriptor_tables.h>
#include <kernel/x86/ioport.h>
#include <kernel/x86/platforms/pc/pit.h>

extern kernel::driver::uart uart;

// Board facts for the PC: the legacy ISA device set that every x86_64 machine
// inherits. The counter itself (rdtsc) is a CPU register and lives in
// x86_64/arch.cpp; only its rate is established here, because that depends on
// the board's tick source.
namespace kernel::platform {

namespace {
uint64_t g_tsc_hz             = 0;

// Tick source: the LAPIC timer, calibrated once at boot against the PIT (the
// fixed-frequency reference every PC inherits). Ticks are 1 ms exactly.
constexpr time_ns_t TICK_NS   = 1'000'000;
constexpr unsigned int CAL_MS = 10;

struct lapic_tick_handler : kernel::hal::IInterruptHandler {
    bool handle_interrupt(register_frame_t*) override {
        kernel::time::tick();
        return true;
    }
};
lapic_tick_handler g_timer;
}  // namespace

// The PC console is COM1, reached by port I/O, so nothing needs to be mapped first.
void console_init() { uart.init(); }

// x86 DMA is cache-coherent, so devices see CPU writes with no flush.
void dcache_clean_range(const void*, size_t) {}

void timer_init() {
    // A dead PIT or LAPIC timer degrades to a frozen clock with a warning, matching
    // timestamp_calibrate's dead-timer policy: readers see time stuck at 0, boot continues.
    // ponytail: no fallback tick source -- calibrate from CPUID 0x15 or the hypervisor leaves
    // if legacy-free hardware (no PIT) ever matters.
    kernel::time::init(TICK_NS);
    kernel::x86::lapic_init();
    kernel::x86::lapic_timer_start_counting();
    if (!pc::pit_poll_wait_ms(CAL_MS)) {
        g_log.warn("timer_init: PIT not counting; kernel time will not advance");
        return;
    }
    uint32_t counts = kernel::x86::lapic_timer_elapsed();
    if (counts < CAL_MS) {
        g_log.warn("timer_init: LAPIC timer did not advance; kernel time will not advance");
        return;
    }

    g_interrupt_manager.register_interrupt(kernel::x86::IRQ0, &g_timer, 0);
    kernel::x86::lapic_timer_start_periodic(kernel::x86::IRQ0, counts / CAL_MS);
    g_log.info("Time subsystem initialized (LAPIC timer, {0} counts/ms)", counts / CAL_MS);
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

    // Invariant TSC (CPUID 0x80000007 EDX bit 8) means the counter runs at a constant rate across
    // P-/C-state changes. Without it the calibrated rate can drift; warn but keep the clock --
    // QEMU TCG does not advertise the bit yet ticks the TSC at a fixed virtual rate.
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000000u), "c"(0u));
    if (eax >= 0x80000007u) {
        asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000007u), "c"(0u));
        if ((edx & (1u << 8)) == 0) { g_log.warn("platform: TSC is not invariant; timestamps may drift"); }
    }
}

// The PC target runs under QEMU, which has no watchdog worth arming.
void watchdog_arm() {}
void watchdog_init() {}

// Pulse the 8042 keyboard controller's reset line, the legacy reboot path
// every PC inherits.
void reboot() { outb(0x64, 0xFE); }

// The firmware memory map is complete; nothing is fenced off.
uint64_t firmware_fenced_memory_base() { return 0; }

}  // namespace kernel::platform
