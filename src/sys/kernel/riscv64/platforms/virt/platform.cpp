#include <kernel/drivers/uart.h>
#include <kernel/log.h>
#include <kernel/platform.h>
#include <kernel/riscv/platforms/virt/timer.h>

extern kernel::driver::uart uart;

// Board facts for QEMU's riscv64 `virt` machine. The counter itself (rdtime)
// is a CPU register and lives in riscv64/arch.cpp; its rate is the board's
// timebase, so it lives here.
//
// MMIO devices are reached through the HHDM (the bootloader maps at least the
// first 4 GiB of physical space there); published by riscv64/main.cpp at boot.
extern uintptr_t g_hhdm_offset;

namespace kernel::platform {

namespace {
// virt's sifive_test finisher device. A 32-bit write of FINISHER_PASS exits
// QEMU with status 0; (code << 16) | FINISHER_FAIL exits with `code`.
constexpr uintptr_t SIFIVE_TEST_PADDR = 0x100000;
constexpr uint32_t FINISHER_PASS      = 0x5555;
constexpr uint32_t FINISHER_FAIL      = 0x3333;

// virt's timebase-frequency, matching the SBI timer driver's hardcode. Reading
// the DTB's /cpus/timebase-frequency is what real hardware needs (VisionFive 2
// is 4 MHz) and is tracked as a todo shared with the timer.
constexpr uint64_t TIMEBASE_FREQ_HZ   = 10'000'000;

// SBI-backed tick source. Interrupt-handler object, so its constructor must
// have run (global ctors) before timer_init().
virt::sbi_timer g_timer;
}  // namespace

// virt's console is a 16550 at a fixed MMIO address, reached through the HHDM,
// so resolve_hhdm() must have run before this.
void console_init() {
    uart.init();
    g_log.devices.push_back(&uart);
}

// No fixed interrupt hardware needs quiescing; PLIC routing is future work.
void interrupt_init() {}

void timer_init() { g_timer.init(); }

void harness_exit(uint8_t code) {
    if (g_hhdm_offset == 0) { return; }  // MMIO unreachable before the HHDM is known
    volatile uint32_t* finisher = reinterpret_cast<volatile uint32_t*>(g_hhdm_offset + SIFIVE_TEST_PADDR);
    *finisher                   = code == 0 ? FINISHER_PASS : ((static_cast<uint32_t>(code) << 16) | FINISHER_FAIL);
}

uint64_t timestamp_hz() { return TIMEBASE_FREQ_HZ; }

// The timebase is a fixed board constant, so there is nothing to measure.
void timestamp_calibrate() {}

}  // namespace kernel::platform
