#include "kernel/arch.h"

#include "kernel/log.h"
#include "kernel/panic.h"
#include "kernel/time.h"
#include "kernel/x86/cpu.h"
#include "kernel/x86/descriptor_tables.h"
#include "kernel/x86/ioport.h"

extern "C" uint64_t g_syscall_kernel_rsp;
extern "C" void syscall_entry();

namespace {
constexpr uint32_t MSR_EFER  = 0xC0000080;
constexpr uint32_t MSR_STAR  = 0xC0000081;
constexpr uint32_t MSR_LSTAR = 0xC0000082;
constexpr uint32_t MSR_FMASK = 0xC0000084;

// RFLAGS bits cleared by SYSCALL on kernel entry: anything a user thread can set that would change
// how kernel code executes. NT is the subtle one -- left set, an IRET taken during the syscall is
// treated as a task-switch return rather than a plain one.
constexpr uint32_t RFLAGS_TF = 1u << 8;
constexpr uint32_t RFLAGS_IF = 1u << 9;
constexpr uint32_t RFLAGS_DF = 1u << 10;
constexpr uint32_t RFLAGS_NT = 1u << 14;
constexpr uint32_t RFLAGS_RF = 1u << 16;
constexpr uint32_t RFLAGS_AC = 1u << 18;
}  // namespace

[[noreturn]] void hcf() {
    asm volatile("cli");
    for (;;) { asm volatile("hlt"); }
}

namespace kernel::arch {

void set_kernel_stack(uintptr_t top) {
    kernel::x86::set_tss_rsp0(top);
    g_syscall_kernel_rsp = top;
}

void syscall_init() {
    x86::wrmsr(MSR_EFER, x86::rdmsr(MSR_EFER) | 1);
    x86::wrmsr(MSR_STAR, (0x13ULL << 48) | (0x08ULL << 32));
    x86::wrmsr(MSR_LSTAR, reinterpret_cast<uint64_t>(&syscall_entry));
    x86::wrmsr(MSR_FMASK, RFLAGS_TF | RFLAGS_IF | RFLAGS_DF | RFLAGS_NT | RFLAGS_RF | RFLAGS_AC);
}

[[noreturn]] void enter_user(uintptr_t entry, uintptr_t user_sp, uintptr_t kstack_top, uintptr_t ipc_base,
                             uintptr_t ipc_size) {
    disable_interrupts();
    set_kernel_stack(kstack_top);
    // rdi/rsi carry the IPC buffer, matching the SysV argument registers so startup code reads them
    // as ordinary function arguments. SYSRET takes the entry point from rcx and RFLAGS from r11.
    // Everything outside that ABI is zeroed so no kernel value stays readable in a register. The
    // operands are pinned to fixed registers so the clearing cannot overwrite one the compiler
    // placed elsewhere; rax holds the user stack until last because rsp cannot be an asm operand.
    asm volatile(
        "movq $0x202, %%r11\n"
        "movq %%rax, %%rsp\n"
        "xorl %%eax, %%eax\n"
        "xorl %%ebx, %%ebx\n"
        "xorl %%edx, %%edx\n"
        "xorl %%ebp, %%ebp\n"
        "xorl %%r8d, %%r8d\n"
        "xorl %%r9d, %%r9d\n"
        "xorl %%r10d, %%r10d\n"
        "xorl %%r12d, %%r12d\n"
        "xorl %%r13d, %%r13d\n"
        "xorl %%r14d, %%r14d\n"
        "xorl %%r15d, %%r15d\n"
        "sysretq\n"
        : "+c"(entry), "+a"(user_sp), "+D"(ipc_base), "+S"(ipc_size)
        :
        : "rbx", "rdx", "rbp", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "memory");
    __builtin_unreachable();
}

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

uint64_t timestamp() {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

}  // namespace kernel::arch
