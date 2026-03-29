# Interrupt Model

This page documents the current interrupt subsystem and the planned object-model direction.

## Current System
The interrupt manager maintains a table of handler entries.
Each entry can hold either a function pointer or an object pointer, with flags for enable state and interface type.
The manager tracks per-CPU reentrant state to detect nested interrupts.

The PIT timer is the primary example -- it increments the kernel tick counter on each interrupt.

## Planned Architecture
The long-term design treats interrupts as handle-bearing [[Object Model|kernel objects]], unifying interrupt management with the rest of the system.

In the planned model:

1. Hardware interrupt fires
2. Kernel captures and masks the interrupt
3. Kernel signals the interrupt object (sets signal bits)
4. Driver server's wait on the handle unblocks
5. Server handles the interrupt and signals back to unmask

[[Object Transaction Programs]] would apply to interrupt objects like any other type -- for example, auto-acknowledging edge-triggered interrupts without IPC, or coalescing signals for bursty sources.

**Exceptions to the object model**: Timer interrupts stay in-kernel.
The scheduler and watchdogs depend on them -- routing through IPC would risk deadlock.
The emergency serial driver also stays in-kernel for diagnostics independence.
