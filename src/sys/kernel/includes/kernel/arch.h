#pragma once

#include <stddef.h>
#include <stdint.h>

// Arch-neutral CPU and platform primitives, implemented by each architecture in its own
// directory. Generic code (core/, mm/, std/, shell/) must use these instead of inline
// assembly; the host test runner stubs them, so they must stay out-of-line.
namespace kernel::arch {

/// Dense logical index of the calling CPU in [0, CONFIG_MAX_CORES).
size_t current_core_index();

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
/// Its rate is a board property; see kernel::platform::timestamp_hz().
uint64_t timestamp();

/// Per-thread user FP/SIMD register state, saved and restored across context switches. The size
/// and alignment fit the largest per-arch format: the x86_64 FXSAVE area uses all 512 bytes, the
/// riscv64 f-register file plus fcsr uses the first 260.
inline constexpr size_t FPU_AREA_SIZE  = 512;
inline constexpr size_t FPU_AREA_ALIGN = 16;
/// Fill `area` with the architecture's program-entry FP state: registers clear, exceptions masked.
void fpu_init(void* area);
void fpu_save(void* area);
void fpu_restore(void* area);

/// Publish the current kernel stack top for user-to-kernel transitions.
void set_kernel_stack(uintptr_t top);
/// Drops to user mode at `entry`; never returns. The thread's IPC buffer base and size arrive in the
/// first two argument registers, which is how a thread learns where its buffer is -- no address is
/// baked into the ABI, so user-space ASLR can move it later without an ABI break.
[[noreturn]] void enter_user(uintptr_t entry, uintptr_t user_sp, uintptr_t kstack_top, uintptr_t ipc_base,
                             uintptr_t ipc_size);

}  // namespace kernel::arch

/// Callee-saved context switch: pushes callee-saved registers on the current stack, stores the
/// resulting sp to *save_sp, loads load_sp, pops, returns on the new stack. Interrupts must be
/// disabled by the caller.
extern "C" void arch_context_switch(uintptr_t* save_sp, uintptr_t load_sp);
