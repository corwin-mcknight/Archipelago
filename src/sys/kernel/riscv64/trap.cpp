#include <kernel/crash.h>
#include <kernel/interrupt.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/platform.h>
#include <kernel/registers.h>
#include <kernel/sched/user_task.h>
#include <kernel/synchronization/execution_context.h>
#include <kernel/syscall.h>

#include "kernel/mm/vm_aspace.h"

extern "C" void riscv_trap_entry();
extern "C" [[noreturn]] void riscv_trap_stack_overflow(uintptr_t sepc, uintptr_t stval);
extern "C" void riscv_trap_handler(register_frame_t* regs);

namespace kernel::riscv { void trap_init(); }

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

namespace kernel::riscv {

void trap_init() {
    // Direct mode: all traps vector to one entry (address low bits 00).
    asm volatile("csrw stvec, %0" ::"r"(&riscv_trap_entry));
    asm volatile("csrw sscratch, zero");

    // Allow FP execution: sstatus.FS resets to Off, where any FP touch -- user code or the
    // scheduler's own state switching -- traps as an illegal instruction. Setting Initial (0b01)
    // once per hart is enough: hardware only ever promotes FS toward Dirty, every other sstatus
    // write in the kernel is bit-scoped or round-trips a live value, and the trap return path
    // restores the frame's saved sstatus, so no path can turn FS back Off. It lives here so every
    // hart inherits FP enablement along with its trap vector.
    constexpr uint64_t SSTATUS_FS_INITIAL = 1ull << 13;
    asm volatile("csrs sstatus, %0" ::"r"(SSTATUS_FS_INITIAL));
}

}  // namespace kernel::riscv

extern "C" [[noreturn]] void riscv_trap_stack_overflow(uintptr_t sepc, uintptr_t stval) {
    g_log.error("trap: kernel stack overflow, sepc=0x{0:p} stval=0x{1:p}", sepc, stval);
    panic("kernel stack overflow (recursive trap)");
}

extern "C" void riscv_trap_handler(register_frame_t* regs) {
    if (regs->scause & SCAUSE_INTERRUPT) {
        kernel::synchronization::interrupt_enter();
        const auto cause = static_cast<unsigned int>(regs->scause & ~SCAUSE_INTERRUPT);
        if (cause != 9 || !kernel::platform::dispatch_external_interrupt(regs)) {
            g_interrupt_manager.dispatch_interrupt(cause, regs);
        }
        kernel::synchronization::interrupt_exit();
        return;
    }

    // Keep synchronous traps in the same execution-context state as x86_64. In particular, the
    // user-fault handoff below must explicitly leave fault context before it schedules away.
    kernel::synchronization::fault_enter();

    // Page faults get one shot at demand-paging resolution before the crash
    // path; an unresolvable fault falls through with diagnostics intact.
    if (is_page_fault(regs->scause)) {
        if (try_resolve_page_fault(regs)) {
            kernel::synchronization::fault_exit();
            return;
        }
    }

    constexpr uint64_t CAUSE_ECALL_U = 8;
    if (regs->scause == CAUSE_ECALL_U) {
        kernel::synchronization::fault_exit();
        // User ABI: a7 = number, a0..a5 = args. The trap frame already saves them all, and riscv64
        // has enough argument registers to pass every one of them plus the number.
        regs->a0 = syscall_dispatch(regs->a7, regs->a0, regs->a1, regs->a2, regs->a3, regs->a4, regs->a5);
        regs->sepc += 4;
        return;
    }

    if ((regs->sstatus & SSTATUS_SPP) == 0) {
        kernel::synchronization::fault_exit();
        kernel::sched::terminate_current_user_task_from_fault(regs->scause, regs->stval, regs->sepc);
    }

    kernel::crash::dispatch(kernel::crash::trigger_kind::exception, regs);
}
