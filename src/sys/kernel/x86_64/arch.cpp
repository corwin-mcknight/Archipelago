#include "kernel/arch.h"

#include "kernel/panic.h"
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

}  // namespace kernel::arch
