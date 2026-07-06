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

}  // namespace kernel::arch
