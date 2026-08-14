#include "kernel/arch.h"

#include "kernel/panic.h"

namespace {
// sstatus.SIE: supervisor interrupt enable.
constexpr uint64_t SSTATUS_SIE = 1ull << 1;
}  // namespace

[[noreturn]] void hcf() {
    asm volatile("csrci sstatus, %0" ::"i"(SSTATUS_SIE));
    for (;;) { asm volatile("wfi"); }
}

namespace kernel::arch {

// The trap return path republishes the frame's kernel stack top in sscratch.
void set_kernel_stack(uintptr_t) {}

[[noreturn]] void enter_user(uintptr_t entry, uintptr_t user_sp, uintptr_t kstack_top, uintptr_t ipc_base,
                             uintptr_t ipc_size) {
    disable_interrupts();
    constexpr uint64_t SSTATUS_SPP  = 1ull << 8;
    constexpr uint64_t SSTATUS_SPIE = 1ull << 5;
    // a0/a1 carry the IPC buffer, matching the argument registers so startup code reads them as
    // ordinary function arguments. Everything outside that ABI is zeroed so no kernel value stays
    // readable in a register; the zeroing follows the last use of every operand, and the operands
    // are read-write because it overwrites them. gp and tp are safe to clear: the kernel is built
    // -mcmodel=medany and the trap path restores both from the frame rather than assuming a value.
    asm volatile(
        "csrw sscratch, %0\n"
        "li t0, %5\n"
        "csrc sstatus, t0\n"
        "li t0, %6\n"
        "csrs sstatus, t0\n"
        "csrw sepc, %1\n"
        "mv sp, %2\n"
        "mv a0, %3\n"
        "mv a1, %4\n"
        "li ra, 0\n"
        "li gp, 0\n"
        "li tp, 0\n"
        "li t0, 0\n"
        "li t1, 0\n"
        "li t2, 0\n"
        "li t3, 0\n"
        "li t4, 0\n"
        "li t5, 0\n"
        "li t6, 0\n"
        "li s0, 0\n"
        "li s1, 0\n"
        "li s2, 0\n"
        "li s3, 0\n"
        "li s4, 0\n"
        "li s5, 0\n"
        "li s6, 0\n"
        "li s7, 0\n"
        "li s8, 0\n"
        "li s9, 0\n"
        "li s10, 0\n"
        "li s11, 0\n"
        "li a2, 0\n"
        "li a3, 0\n"
        "li a4, 0\n"
        "li a5, 0\n"
        "li a6, 0\n"
        "li a7, 0\n"
        "sret\n"
        : "+r"(kstack_top), "+r"(entry), "+r"(user_sp), "+r"(ipc_base), "+r"(ipc_size)
        : "i"(SSTATUS_SPP), "i"(SSTATUS_SPIE)
        : "ra", "gp", "tp", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
          "s8", "s9", "s10", "s11", "a0", "a1", "memory");
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

// User programs are integer-only (rv64imac, sstatus.FS off): there is no FP state to carry until
// the F/D extensions are enabled for user mode.
void fpu_init(void*) {}
void fpu_save(void*) {}
void fpu_restore(void*) {}

}  // namespace kernel::arch
