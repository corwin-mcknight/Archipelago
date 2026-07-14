# Kernel Locking
Lock ownership, preemption control, and interrupt masking are separate operations. Code chooses the narrowest guard that matches the data's access contexts.

## Guards
`lock_guard<Lock>` calls only `lock()` and `unlock()`. It neither disables preemption nor masks interrupts, so it is appropriate for blocking mutexes and for other generic lock-compatible types whose context requirements are already satisfied.

`critical_section` prevents context switches while leaving local interrupts enabled. `critical_irq_section` additionally masks local interrupts and restores the exact prior interrupt state. `critical_lock_guard` and `critical_irq_lock_guard` combine those context guards with a generic lock guard; destruction releases the lock before restoring the execution context.

## Spinlocks
A spinlock requires preemption to be disabled before acquisition. Thread- and fault-only spinlocks use `critical_lock_guard`. A lock shared with interrupt handlers uses `critical_irq_lock_guard` so the local handler cannot re-enter it. Scheduler, wait-queue, PMM, and VMM internals remain spinlock-based because they may execute where blocking is unsafe.

## Mutexes
The kernel mutex is non-recursive, non-fair, and may be used only from preemptible thread context. Its uncontended compare-and-swap path is also available during single-threaded boot. Contention before scheduler startup is a fatal error; after startup, contenders park on the mutex's embedded wait queue. Unlock publishes the unlocked state and wakes one waiter, but does not hand ownership directly to it, so barging is permitted.

Handle tables, tasks, the task registry, and the object type registry use mutexes. Priority inheritance, cancellation, timeouts, and fairness are not provided.

## Execution Context and Deferred Preemption
Each CPU records nesting depths for preemption-disabled sections, IRQ-masked sections, interrupts, faults, and syscalls, together with the current thread identity. A timer tick always performs timekeeping, wakeups, slice accounting, and trace updates. If a switch is not currently eligible, it records one pending preemption; leaving the outermost eligible context consumes that request and performs at most one reschedule.

Yielding, sleeping, blocking, exiting, and direct scheduling require preemption to be enabled and no locks to be held.

## Debug Lock Dependency Checking
Debug builds track up to 16 locks held by each CPU, 128 registered instrumented locks, and 512 learned dependency edges without allocating memory. Every nested acquisition learns edges from held locks to the new lock. An acquisition that closes a dependency cycle, recursive acquisition, non-owner or out-of-order release, and capacity exhaustion are fatal diagnostics.

Spinlocks and mutexes have stable monotonic lock identities. Generic lock-compatible types are still checked for balanced LIFO release and same-stack recursion by address, but do not participate in the persistent dependency graph. Lock ownership and graph metadata compile out when `NDEBUG` is defined.
