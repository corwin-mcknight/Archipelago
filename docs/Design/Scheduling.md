# Scheduling

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

Today these are direct kernel calls -- there is no userspace yet, so there is no syscall boundary to cross.
Once userspace exists, these operations will go through the [[Syscall Interface]] like any other kernel interaction.
A blocked thread is removed from the run queue and re-added when the blocking condition is satisfied.

## Context Switching
A voluntary context switch saves and restores only the callee-saved register set of the outgoing and incoming threads.
A preempted thread's full register state is already preserved in its trap frame, so it resumes by returning out through the interrupt return path rather than through the switch primitive.
Context switches occur at two points today:
- **Voluntary** -- yield, block, and exit switch away directly
- **Preemption** -- when the timer fires and the scheduler selects a different thread

A syscall boundary becomes a third context switch point once userspace exists.
Preemption is tick-driven: each timer interrupt decrements the running thread's remaining timeslice, and the thread is switched out once it reaches zero.

## Task Zero
The kernel itself is [[Task Model#The Kernel as Task Zero|task zero]].
It owns the kernel's threads and the kernel's handle table, so every thread the kernel spawns for its own purposes -- the idle thread, the reaper, the shell -- lives under task zero rather than a separate global structure.

## Idle Thread
When no threads are runnable, the kernel runs its idle thread.
The kernel is task zero and must be schedulable for this reason.
The context executing at boot becomes the boot processor's idle thread directly, with no stack handoff, and falls into the idle loop once the reaper and initial threads are spawned.

## Thread Lifecycle and the Reaper
An exiting thread marks itself dead, signals its termination, and switches away without freeing its own stack -- a thread cannot safely tear down the stack it is still running on.
A dedicated reaper thread blocks until a thread has exited, then reclaims the dead thread's object and stack.
This keeps exit cheap and keeps stack reclamation off the hot path of every thread's own exit.

## Wait Queues and Waitable Signals
Blocking is built on a single primitive: a wait queue that holds threads until woken, either individually or all at once.
Sleeping for a fixed number of ticks and waiting on an object's signals are both built on top of this primitive.
Kernel objects expose signals as waitable conditions -- a thread can block until a specific signal becomes set rather than polling.
Thread join is an instance of this: waiting for a thread's termination signal.

## Stack Overflow Protection
Each thread runs on a fixed-size kernel stack.
The scheduler publishes the floor of the incoming thread's stack on every context switch, and trap entry checks the stack pointer against that floor before doing any further work.
A violation diverts to a dedicated emergency stack and panics through the crash-reporting path rather than corrupting adjacent memory.
This is a tripwire, not a hardware guard page -- it catches the realistic failure (deep recursion) without requiring guard-paged stack mappings.

## Multi-Core Scope
Scheduler state is shaped per-CPU, but only the boot processor runs a scheduler today.
Application processors continue to park; bringing them into scheduling is future work that needs a per-CPU timer, per-CPU core-identity pointers, and cross-core wake paths, without reshaping the scheduler's data structures.

## Observability
The scheduler keeps per-thread accounting -- CPU time, scheduling counts, and wait latency -- alongside an always-on bounded event trace and an optional lifecycle log stream.
All of it surfaces through the kernel shell.
Trace detail is pulled on demand rather than streamed continuously, because the serial wire cannot carry per-switch logging at the rate threads actually switch.
This keeps the scheduler inspectable without adding overhead or bandwidth pressure to the hot path.

## Relationship to Other Subsystems
- [[Task Model]] -- threads live inside tasks; the scheduler operates on threads
- [[Syscall Interface]] -- yield and block become syscalls once userspace exists, adding syscall boundaries as a third context switch point
- [[Interrupt Model]] -- the timer interrupt drives preemption
