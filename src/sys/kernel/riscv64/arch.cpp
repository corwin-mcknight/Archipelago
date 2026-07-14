#include "kernel/arch.h"

#include "kernel/panic.h"

// MMIO devices are reached through the HHDM (Limine maps at least the first
// 4 GiB of physical space there); published by riscv64/main.cpp at boot.
extern uintptr_t g_hhdm_offset;

namespace {
// sstatus.SIE: supervisor interrupt enable.
constexpr uint64_t SSTATUS_SIE        = 1ull << 1;

// QEMU virt's sifive_test finisher device. A 32-bit write of FINISHER_PASS
// exits QEMU with status 0; (code << 16) | FINISHER_FAIL exits with `code`.
constexpr uintptr_t SIFIVE_TEST_PADDR = 0x100000;
constexpr uint32_t FINISHER_PASS      = 0x5555;
constexpr uint32_t FINISHER_FAIL      = 0x3333;
}  // namespace

[[noreturn]] void hcf() {
    asm volatile("csrci sstatus, %0" ::"i"(SSTATUS_SIE));
    for (;;) { asm volatile("wfi"); }
}

namespace kernel::arch {

// The trap return path republishes the frame's kernel stack top in sscratch.
void set_kernel_stack(uintptr_t) {}

[[noreturn]] void enter_user(uintptr_t entry, uintptr_t user_sp, uintptr_t kstack_top) {
    disable_interrupts();
    constexpr uint64_t SSTATUS_SPP  = 1ull << 8;
    constexpr uint64_t SSTATUS_SPIE = 1ull << 5;
    asm volatile(
        "csrw sscratch, %0\n"
        "csrc sstatus, %1\n"
        "csrs sstatus, %2\n"
        "csrw sepc, %3\n"
        "mv sp, %4\n"
        "sret\n"
        :
        : "r"(kstack_top), "r"(SSTATUS_SPP), "r"(SSTATUS_SPIE), "r"(entry), "r"(user_sp)
        : "memory");
    __builtin_unreachable();
}

void enable_interrupts() { asm volatile("csrsi sstatus, %0" ::"i"(SSTATUS_SIE)); }
void disable_interrupts() { asm volatile("csrci sstatus, %0" ::"i"(SSTATUS_SIE)); }

bool interrupts_enabled() {
    uint64_t sstatus;
    asm volatile("csrr %0, sstatus" : "=r"(sstatus)::"memory");
    return (sstatus & SSTATUS_SIE) != 0;
}

uint64_t save_and_disable_interrupts() {
    uint64_t sstatus;
    asm volatile("csrrci %0, sstatus, %1" : "=r"(sstatus) : "i"(SSTATUS_SIE) : "memory");
    return sstatus;
}

void restore_interrupts(uint64_t flags) {
    if (flags & SSTATUS_SIE) {
        asm volatile("csrsi sstatus, %0" ::"i"(SSTATUS_SIE) : "memory");
    } else {
        asm volatile("csrci sstatus, %0" ::"i"(SSTATUS_SIE) : "memory");
    }
}

uintptr_t active_translation_root() {
    uint64_t satp;
    asm volatile("csrr %0, satp" : "=r"(satp));
    return (satp & ((1ull << 44) - 1)) << 12;  // PPN field -> physical address
}

void harness_exit(uint8_t code) {
    if (g_hhdm_offset == 0) { return; }  // MMIO unreachable before the HHDM is known
    volatile uint32_t* finisher = reinterpret_cast<volatile uint32_t*>(g_hhdm_offset + SIFIVE_TEST_PADDR);
    *finisher                   = code == 0 ? FINISHER_PASS : ((static_cast<uint32_t>(code) << 16) | FINISHER_FAIL);
}

[[noreturn]] void trigger_invalid_opcode() {
    asm volatile("unimp");
    hcf();  // the illegal-instruction handler never returns; this only satisfies [[noreturn]]
}

[[noreturn]] void trigger_breakpoint() {
    asm volatile("ebreak");
    hcf();
}

void wait_for_interrupt() { asm volatile("wfi"); }

extern "C" void thread_entry_trampoline();

uintptr_t prepare_thread_stack(uintptr_t stack_top, void (*entry)(void*), void* arg) {
    // Mirrors the 112-byte frame in context_switch.S: ra slot 0, s1 slot 2, s2 slot 3.
    uintptr_t sp = stack_top - 112;
    uint64_t* f  = reinterpret_cast<uint64_t*>(sp);
    for (int i = 0; i < 14; ++i) { f[i] = 0; }
    f[0] = reinterpret_cast<uint64_t>(&thread_entry_trampoline);  // ra
    f[2] = reinterpret_cast<uint64_t>(entry);                     // s1
    f[3] = reinterpret_cast<uint64_t>(arg);                       // s2
    return sp;
}

uint64_t timestamp() {
    uint64_t t;
    asm volatile("rdtime %0" : "=r"(t));
    return t;
}

// QEMU virt's timebase, matching the SBI timer driver's hardcode; DTB-driven discovery for
// real hardware (VisionFive 2: 4 MHz) is an existing todo shared with the timer.
uint64_t timestamp_hz() { return 10'000'000; }

void timestamp_calibrate() {}

}  // namespace kernel::arch
