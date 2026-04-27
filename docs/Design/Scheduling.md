# Scheduling

> [!info] Design
> This feature is not yet implemented. This page describes the planned design.

The kernel owns the scheduler.
Scheduling policy is not exposed to userspace and is not pluggable.

## Current Design
The scheduler uses round-robin rotation across all runnable threads.
On each timer tick or voluntary yield, the kernel picks the next runnable thread and switches to it.
There is no priority system, no fairness weights, and no scheduling classes.

This is intentionally minimal.
A more sophisticated scheduler is a future optimization, not a design-time concern.
Round-robin is sufficient to reach the first userspace milestone and validate the rest of the architecture.

## Thread Control
Threads interact with the scheduler through their own handles:
- **Yield** -- voluntarily give up the current timeslice
- **Block** -- suspend until a condition is met (signal wait, futex, etc.)

These operations go through the [[Syscall Interface]] like any other kernel interaction.
A blocked thread is removed from the run queue and re-added when the blocking condition is satisfied.

## Context Switching
A context switch saves and restores the full register state of the outgoing and incoming threads.
Context switches occur at two points:
- **Syscall boundary** -- when a thread enters or exits the kernel
- **Preemption** -- when the timer fires and the scheduler selects a different thread

## Idle Thread
When no threads are runnable, the kernel runs its idle thread.
The kernel is [[Task Model#The Kernel as Task Zero|task zero]] and must be schedulable for this reason.

## Relationship to Other Subsystems
- [[Task Model]] -- threads live inside tasks; the scheduler operates on threads
- [[Syscall Interface]] -- yield and block are syscalls; syscall boundaries are context switch points
- [[Interrupt Model]] -- the timer interrupt drives preemption
