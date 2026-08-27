#include <kernel/drivers/uart.h>
#include <kernel/log.h>
#include <kernel/mm/mmio.h>
#include <kernel/mm/physmap.h>
#include <kernel/platform.h>

extern kernel::driver::uart uart;

// Board facts for QEMU's riscv64 `virt` machine. The counter itself (rdtime)
// is a CPU register and lives in riscv64/arch.cpp; its rate is the board's
// timebase, so it lives here.
//
// MMIO devices are reached through the HHDM (the bootloader maps at least the
// first 4 GiB of physical space there); published by riscv64/main.cpp at boot.
namespace kernel::platform {

namespace {
// virt's sifive_test finisher device. A 32-bit write of FINISHER_PASS exits
// QEMU with status 0; (code << 16) | FINISHER_FAIL exits with `code`.
constexpr uintptr_t SIFIVE_TEST_PADDR = 0x100000;
constexpr uint32_t FINISHER_PASS      = 0x5555;
constexpr uint32_t FINISHER_FAIL      = 0x3333;

// virt's timebase-frequency; the shared SBI timer derives its tick interval
// from this via timestamp_hz(). Reading the DTB's /cpus/timebase-frequency
// instead of hardcoding per board is tracked as a todo.
constexpr uint64_t TIMEBASE_FREQ_HZ   = 10'000'000;

}  // namespace

// virt's console is a 16550 at a fixed MMIO address, reached through the HHDM,
// so resolve_hhdm() must have run before this.
void console_init() { uart.init(); }

// No fixed interrupt hardware needs quiescing; PLIC routing is future work.
void interrupt_init() {}

bool dispatch_external_interrupt(::register_frame*) { return false; }

void interrupt_set_source_enabled(unsigned int, bool) {}

unsigned int console_uart_interrupt_id() { return 0; }
int console_input_read() { return -1; }

void harness_exit(uint8_t code) {
    if (!kernel::mm::direct_map_ready()) { return; }
    auto finisher = kernel::mm::map_mmio({kernel::mm::physical_address(SIFIVE_TEST_PADDR), sizeof(uint32_t)});
    finisher.write32(0, code == 0 ? FINISHER_PASS : ((static_cast<uint32_t>(code) << 16) | FINISHER_FAIL));
}

uint64_t timestamp_hz() { return TIMEBASE_FREQ_HZ; }

// The timebase is a fixed board constant, so there is nothing to measure.
void timestamp_calibrate() {}

// The firmware memory map is complete; nothing is fenced off.
uint64_t firmware_fenced_memory_base() { return 0; }

// QEMU presents a cache-coherent machine, so DMA sees CPU writes with no flush.
void dcache_clean_range(const void*, size_t) {}

// virt has no watchdog device.
void watchdog_arm() {}
void watchdog_init() {}

// SBI SRST extension (EID "SRST", FID 0): ask the SBI firmware to cold-reboot
// the machine. Returns if the firmware lacks the extension.
void reboot() {
    register uint64_t a0 asm("a0") = 1;  // reset type: cold reboot
    register uint64_t a1 asm("a1") = 0;  // reset reason: none
    register uint64_t a6 asm("a6") = 0;
    register uint64_t a7 asm("a7") = 0x53525354;
    asm volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a6), "r"(a7) : "memory");
}

}  // namespace kernel::platform
