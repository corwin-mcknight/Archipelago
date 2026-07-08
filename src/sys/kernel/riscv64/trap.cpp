#include <kernel/crash.h>
#include <kernel/interrupt.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/registers.h>

#include "kernel/mm/vm_aspace.h"

extern "C" void riscv_trap_entry();

namespace {

constexpr uint64_t SCAUSE_INTERRUPT             = 1ull << 63;
constexpr uint64_t SSTATUS_SPP                  = 1ull << 8;  // previous privilege: 0 = user, 1 = supervisor

constexpr uint64_t CAUSE_INSTRUCTION_PAGE_FAULT = 12;
constexpr uint64_t CAUSE_LOAD_PAGE_FAULT        = 13;
constexpr uint64_t CAUSE_STORE_PAGE_FAULT       = 15;

bool is_page_fault(uint64_t cause) {
    return cause == CAUSE_INSTRUCTION_PAGE_FAULT || cause == CAUSE_LOAD_PAGE_FAULT || cause == CAUSE_STORE_PAGE_FAULT;
}

// riscv page faults carry no present/permission distinction in scause, so
// recover it by asking the active space whether a translation exists.
bool try_resolve_page_fault(register_frame_t* regs) {
    auto* space  = kernel::mm::vm_aspace::active();
    bool present = space != nullptr && space->walk(regs->stval).has_value();

    kernel::mm::vm_fault fault{
        .vaddr   = regs->stval,
        .write   = regs->scause == CAUSE_STORE_PAGE_FAULT,
        .present = present,
        .user    = (regs->sstatus & SSTATUS_SPP) == 0,
    };
    return kernel::mm::vmm_handle_fault(fault);
}

}  // namespace

// Low-water mark for the trap entry's stack-overflow backstop. Single kernel
// stack this milestone; per-thread stacks need this to move per-CPU.
extern "C" uintptr_t g_kstack_floor = 0;

namespace kernel::riscv {

void trap_init() {
    // Direct mode: all traps vector to one entry (address low bits 00).
    asm volatile("csrw stvec, %0" ::"r"(&riscv_trap_entry));

    // Limine guarantees at least 64 KiB of boot stack below the entry sp;
    // trap_init runs near the top of it, so 48 KiB down is legitimately
    // reachable and the last 16 KiB is the overflow tripwire.
    uintptr_t sp;
    asm volatile("mv %0, sp" : "=r"(sp));
    g_kstack_floor = sp - 48 * 1024;
}

}  // namespace kernel::riscv

extern "C" [[noreturn]] void riscv_trap_stack_overflow(uintptr_t sepc, uintptr_t stval) {
    g_log.error("trap: kernel stack overflow, sepc=0x{0:p} stval=0x{1:p}", sepc, stval);
    panic("kernel stack overflow (recursive trap)");
}

extern "C" void riscv_trap_handler(register_frame_t* regs) {
    if (regs->scause & SCAUSE_INTERRUPT) {
        // CLINT/PLIC routing is future work; hand the cause code to the
        // dispatcher so registered handlers (e.g. the SBI timer) can claim it.
        g_interrupt_manager.dispatch_interrupt(static_cast<unsigned int>(regs->scause & ~SCAUSE_INTERRUPT), regs);
        return;
    }

    // Page faults get one shot at demand-paging resolution before the crash
    // path; an unresolvable fault falls through with diagnostics intact.
    if (is_page_fault(regs->scause) && try_resolve_page_fault(regs)) { return; }

    kernel::crash::dispatch(kernel::crash::trigger_kind::exception, regs);
}
