#include <kernel/drivers/uart.h>
#include <kernel/log.h>
#include <kernel/platform.h>

extern kernel::driver::uart uart;

// MMIO devices (the composable cache below) are reached through the HHDM,
// published by riscv64/main.cpp at boot.
extern uintptr_t g_hhdm_offset;

// Board facts for the StarFive JH7110 (Orange Pi RV, VisionFive 2). The
// counter itself (rdtime) is a CPU register and lives in riscv64/arch.cpp;
// its rate is the board's timebase, so it lives here.
namespace kernel::platform {

namespace {
// The JH7110's mtime timebase, per its DTS /cpus/timebase-frequency. Reading
// the DTB instead of hardcoding per board is tracked as a todo.
constexpr uint64_t TIMEBASE_FREQ_HZ = 4'000'000;

// SiFive composable-cache (L2) Flush64 register. Writing a 64-byte-aligned
// physical address writes that line back to DRAM and invalidates it in the
// whole cache hierarchy -- the L2 back-probes each core's L1 -- which is how
// the JH7110 keeps non-coherent DMA engines coherent. The U74 has no Zicbom,
// so `cbo.*` is not an option; this register is the mechanism mainline Linux
// uses for `starfive,jh7110-ccache`, and needs nothing from OpenSBI.
constexpr uintptr_t CCACHE_FLUSH64  = 0x2010000 + 0x200;
constexpr size_t CACHE_LINE_BYTES   = 64;

}  // namespace

// UART0 is a DW_apb_uart reached through the HHDM, so resolve_hhdm() must have
// run before this.
void console_init() { uart.init(); }

// No debug-exit device on real hardware, and SBI SRST dead-ends in OpenSBI's
// pm-reset (the PMIC sits on an I2C bus whose clocks U-Boot gates off at EFI
// handoff). The exit code has nowhere to go, but the contract's intent --
// this kernel instance is done, the harness restarts it -- is a reboot here,
// which also spares crash-expected board tests the 60s watchdog wait.
void harness_exit(uint8_t) { reboot(); }

uint64_t timestamp_hz() { return TIMEBASE_FREQ_HZ; }

// The timebase is a fixed board constant, so there is nothing to measure.
void timestamp_calibrate() {}

// vaddr is a physmap pointer; subtract the HHDM offset to recover the physical
// line addresses the Flush64 register expects. One uncached MMIO store per
// 64-byte line, then a fence so the writebacks retire before the next scanout.
void dcache_clean_range(const void* vaddr, size_t bytes) {
    if (g_hhdm_offset == 0 || bytes == 0) { return; }
    uintptr_t phys             = reinterpret_cast<uintptr_t>(vaddr) - g_hhdm_offset;
    volatile uint64_t* flush64 = reinterpret_cast<volatile uint64_t*>(g_hhdm_offset + CCACHE_FLUSH64);
    uintptr_t end              = phys + bytes;
    for (uintptr_t line = phys & ~(uintptr_t)(CACHE_LINE_BYTES - 1); line < end; line += CACHE_LINE_BYTES) {
        *flush64 = line;
    }
    asm volatile("fence" ::: "memory");
}

}  // namespace kernel::platform
