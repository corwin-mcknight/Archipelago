#include "kernel/arch.h"

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

}  // namespace kernel::arch
