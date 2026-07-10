#pragma once

#include <stdint.h>

// Arch-neutral CPU and platform primitives, implemented by each architecture in its own
// directory. Generic code (core/, mm/, std/, shell/) must use these instead of inline
// assembly; the host test runner stubs them, so they must stay out-of-line.
namespace kernel::arch {

void enable_interrupts();
void disable_interrupts();
/// True if interrupts are currently enabled on the calling core.
bool interrupts_enabled();

/// Save the interrupt-enable state and disable interrupts on the calling core.
/// Nesting-safe: pass the returned token to restore_interrupts() to return to exactly
/// the saved state.
uint64_t save_and_disable_interrupts();
void restore_interrupts(uint64_t flags);

/// Physical address of the translation root currently loaded in hardware
/// (CR3 on x86_64, satp on riscv64). For verifying hardware truth against the
/// mm layer's bookkeeping; portable code manages address spaces via vm_aspace.
uintptr_t active_translation_root();

/// Signal test-harness exit through the emulator's debug-exit device. Returns on
/// hardware (no such device), so callers must halt afterwards.
void harness_exit(uint8_t code);

/// Deliberate fault triggers for crash-path testing (shell `crash` command).
[[noreturn]] void trigger_invalid_opcode();
[[noreturn]] void trigger_breakpoint();

/// Halt until the next interrupt (hlt / wfi). Call with interrupts enabled.
void wait_for_interrupt();

/// Fabricate the initial callee-saved switch frame on a fresh stack so the first
/// arch_context_switch into it lands in the arch's entry trampoline, which enables
/// interrupts, calls entry(arg), and falls into sched_thread_exit(). Returns the initial sp.
uintptr_t prepare_thread_stack(uintptr_t stack_top, void (*entry)(void*), void* arg);

/// Raw CPU cycle counter (TSC on x86_64, time CSR on riscv64). Monotonic on the calling core.
uint64_t timestamp();
/// Cycles per second for timestamp(), 0 if not yet calibrated (readers must handle 0).
uint64_t timestamp_hz();
/// Establish timestamp_hz(). Requires a ticking kernel timer and interrupts enabled;
/// called once from late boot before the scheduler starts.
void timestamp_calibrate();

}  // namespace kernel::arch

/// Callee-saved context switch: pushes callee-saved registers on the current stack, stores the
/// resulting sp to *save_sp, loads load_sp, pops, returns on the new stack. Interrupts must be
/// disabled by the caller.
extern "C" void arch_context_switch(uintptr_t* save_sp, uintptr_t load_sp);
