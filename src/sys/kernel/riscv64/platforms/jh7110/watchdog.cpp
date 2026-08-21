#include <kernel/log.h>
#include <kernel/platform.h>
#include <kernel/sched/scheduler.h>

// The JH7110's watchdog is an SP805-style countdown block at physical
// 0x13070000: LOAD decrements at the 24 MHz core clock, the first expiry
// latches an interrupt, and a second full countdown asserts the SoC's
// warm-reset line -- the PMIC stays up, so the board reboots without the
// power button. The SYSCRG gates the block's clocks off at power-on, so
// init must open them first.
//
// MMIO devices are reached through the HHDM; published by riscv64/main.cpp
// at boot.
extern uintptr_t g_hhdm_offset;

namespace kernel::platform {

namespace {
constexpr uintptr_t SYSCRG_PADDR     = 0x13020000;
constexpr uintptr_t WDOG_PADDR       = 0x13070000;

// SYSCRG clock registers sit at base + 4 * index with the gate in bit 31;
// resets are one bit each in the assert words at +0x2F8. Indexes are the
// TRM's, as mirrored by the starfive,jh7110-crg binding: clocks 122 (apb)
// and 123 (core), resets 109 and 110.
constexpr uintptr_t CLK_WDT_APB      = 4 * 122;
constexpr uintptr_t CLK_WDT_CORE     = 4 * 123;
constexpr uint32_t CLK_ENABLE        = 1u << 31;
constexpr uintptr_t RST_ASSERT       = 0x2F8 + 4 * (109 / 32);
constexpr uint32_t RST_WDT_MASK      = (1u << (109 % 32)) | (1u << (110 % 32));

// Watchdog block registers. Writes are ignored unless LOCK holds the key.
constexpr uintptr_t WDT_LOAD         = 0x000;
constexpr uintptr_t WDT_CONTROL      = 0x008;
constexpr uintptr_t WDT_INTCLR       = 0x00C;
constexpr uintptr_t WDT_LOCK         = 0xC00;
constexpr uint32_t UNLOCK_KEY        = 0x1ACCE551;
constexpr uint32_t CONTROL_RUN       = 0x3;  // counter enabled, reset on second expiry

// Reset fires two full countdowns after the last feed. The feed period leaves
// a wide margin so a busy-but-alive scheduler never trips it, while a wedged
// one still resets within a serial-watching developer's patience.
constexpr uint64_t WDT_CORE_HZ       = 24'000'000;
constexpr uint64_t TIMEOUT_S         = 60;
constexpr uint32_t LOAD_COUNT        = static_cast<uint32_t>(TIMEOUT_S * WDT_CORE_HZ / 2);
constexpr uint64_t FEED_PERIOD_TICKS = 10'000;  // 1 tick = 1 ms

volatile uint32_t& reg(uintptr_t paddr) { return *reinterpret_cast<volatile uint32_t*>(g_hhdm_offset + paddr); }

void feed() {
    reg(WDOG_PADDR + WDT_LOCK)   = UNLOCK_KEY;
    reg(WDOG_PADDR + WDT_INTCLR) = 1;
    reg(WDOG_PADDR + WDT_LOAD)   = LOAD_COUNT;
    reg(WDOG_PADDR + WDT_LOCK)   = 0;
}

// Feeding from a scheduled thread, not the timer interrupt, is the point:
// staying fed proves the scheduler still schedules, not merely that
// interrupts fire.
[[noreturn]] void watchdog_thread_main(void*) {
    while (true) {
        feed();
        kernel::sched::sleep_ticks(FEED_PERIOD_TICKS);
    }
}

}  // namespace

void watchdog_arm() {
    if (g_hhdm_offset == 0) { return; }  // MMIO unreachable before the HHDM is known

    reg(SYSCRG_PADDR + CLK_WDT_APB)  = reg(SYSCRG_PADDR + CLK_WDT_APB) | CLK_ENABLE;
    reg(SYSCRG_PADDR + CLK_WDT_CORE) = reg(SYSCRG_PADDR + CLK_WDT_CORE) | CLK_ENABLE;
    reg(SYSCRG_PADDR + RST_ASSERT)   = reg(SYSCRG_PADDR + RST_ASSERT) & ~RST_WDT_MASK;

    reg(WDOG_PADDR + WDT_LOCK)       = UNLOCK_KEY;
    reg(WDOG_PADDR + WDT_CONTROL)    = 0;
    reg(WDOG_PADDR + WDT_INTCLR)     = 1;
    reg(WDOG_PADDR + WDT_LOAD)       = LOAD_COUNT;
    reg(WDOG_PADDR + WDT_CONTROL)    = CONTROL_RUN;
    reg(WDOG_PADDR + WDT_LOCK)       = 0;
}

void watchdog_init() {
    kernel::sched::spawn("watchdog", watchdog_thread_main, nullptr).expect("watchdog: feeder spawn failed");
    g_log.info("watchdog: feeder started ({0}s timeout, fed every {1}s)", TIMEOUT_S, FEED_PERIOD_TICKS / 1000);
}

// SBI SRST is a dead end on this board: OpenSBI's pm-reset needs the PMIC
// over an I2C bus whose clocks U-Boot gates off at EFI handoff. The watchdog
// is a reset line the kernel already owns, so ask it for an immediate expiry.
void reboot() {
    if (g_hhdm_offset == 0) { return; }
    reg(WDOG_PADDR + WDT_LOCK)   = UNLOCK_KEY;
    reg(WDOG_PADDR + WDT_LOAD)   = 1;
    reg(WDOG_PADDR + WDT_INTCLR) = 1;  // reload the counter from LOAD now
    reg(WDOG_PADDR + WDT_LOCK)   = 0;
    // Reset arrives within microseconds if the watchdog is armed; give it
    // ample time, then return so the caller can report a dead reset path.
    for (volatile uint32_t spins = 0; spins < 100'000'000; ++spins) {}
}

}  // namespace kernel::platform
