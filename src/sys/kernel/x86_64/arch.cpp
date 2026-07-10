#include "kernel/arch.h"

#include "kernel/log.h"
#include "kernel/panic.h"
#include "kernel/time.h"
#include "kernel/x86/ioport.h"

[[noreturn]] void hcf() {
    asm volatile("cli");
    for (;;) { asm volatile("hlt"); }
}

namespace kernel::arch {

void enable_interrupts() { asm volatile("sti"); }
void disable_interrupts() { asm volatile("cli"); }

bool interrupts_enabled() {
    uint64_t flags;
    asm volatile("pushfq; pop %0" : "=r"(flags) : : "memory");
    return (flags & (1ULL << 9)) != 0;  // IF = RFLAGS bit 9
}

uint64_t save_and_disable_interrupts() {
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

void restore_interrupts(uint64_t flags) { asm volatile("push %0; popfq" : : "r"(flags) : "memory", "cc"); }

uintptr_t active_translation_root() {
    uintptr_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

// QEMU's isa-debug-exit device, wired to port 0x604 by the test harness. The 0x2000 bias matches
// what the harness expects to decode from QEMU's exit status.
void harness_exit(uint8_t code) { outw(0x604, static_cast<uint16_t>(code | 0x2000)); }

[[noreturn]] void trigger_invalid_opcode() {
    asm volatile("ud2");
    hcf();  // the #UD handler never returns; this only satisfies [[noreturn]]
}

[[noreturn]] void trigger_breakpoint() {
    asm volatile("int $3");
    hcf();
}

void wait_for_interrupt() { asm volatile("hlt"); }

extern "C" void thread_entry_trampoline();

uintptr_t prepare_thread_stack(uintptr_t stack_top, void (*entry)(void*), void* arg) {
    // Mirrors the pop order in context_switch.s: r15, r14, r13, r12, rbx, rbp, ret.
    // stack_top must be 16-aligned so the trampoline's calls keep SysV alignment.
    uint64_t* sp = reinterpret_cast<uint64_t*>(stack_top);
    *--sp        = reinterpret_cast<uint64_t>(&thread_entry_trampoline);  // ret target
    *--sp        = 0;                                                     // rbp
    *--sp        = reinterpret_cast<uint64_t>(entry);                     // rbx
    *--sp        = reinterpret_cast<uint64_t>(arg);                       // r12
    *--sp        = 0;                                                     // r13
    *--sp        = 0;                                                     // r14
    *--sp        = 0;                                                     // r15
    return reinterpret_cast<uintptr_t>(sp);
}

namespace { uint64_t g_tsc_hz = 0; }  // namespace

uint64_t timestamp() {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

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
        g_log.warn("arch: timestamp calibration skipped (timer not ticking)");
        return;
    }
    uint64_t c0 = timestamp();
    ktime_t t0  = kernel::time::now();
    spins       = 0;
    while (kernel::time::now() < t0 + CAL_TICKS && ++spins < SPIN_CAP) { asm volatile("pause"); }
    if (kernel::time::now() < t0 + CAL_TICKS) {
        g_log.warn("arch: timestamp calibration skipped (timer stalled mid-window)");
        return;
    }
    uint64_t c1  = timestamp();
    time_ns_t ns = kernel::time::ktime_to_ns(kernel::time::now() - t0);
    if (ns == 0) {
        g_log.warn("arch: timestamp calibration skipped (no time elapsed)");
        return;
    }
    g_tsc_hz = (c1 - c0) * 1'000'000'000ull / static_cast<uint64_t>(ns);
    g_log.info("arch: timestamp calibrated: {0} MHz", g_tsc_hz / 1'000'000);
}

}  // namespace kernel::arch
