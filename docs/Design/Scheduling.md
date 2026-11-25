# Scheduling

## Current behavior

The kernel schedules **threads from both kernel and userspace tasks** on x86_64 and riscv64. Scheduling policy stays in the kernel. One [FIFO run queue](../../src/sys/kernel/task/scheduler.cpp) shared by all scheduling cores gives round-robin rotation among runnable threads. Running threads may resume on a different core after yielding, blocking, or being preempted. If a queued thread is still switching out on its previous core, the queue picker rotates it to the back until that switch finishes.

### Multicore startup and idle

The boot processor initializes the scheduler and creates an idle thread for each participating core. Each core adopts its own boot context as its idle thread, keeping the existing stack. Secondary cores wait for scheduler initialization, join the scheduler, and start their local timers. An idle core checks the shared queue before waiting for an interrupt. Enqueuing work from a busy core can send a reschedule IPI to wake an idle peer. See [Boot Process](../Kernel/Boot%20Process.md#3-smp-bringup).

### Yield, block, and preempt

Kernel threads call the scheduler directly. A userspace thread can yield or sleep through the [syscall interface](Syscall%20Interface.md), or block explicitly on object signals or a port. A yield leaves the current thread running if there is no eligible replacement. A blocked thread becomes runnable when its wait is satisfied or its deadline expires.

Every scheduling core has a local tick. The boot core alone advances the shared kernel tick count. Each core's tick checks the shared sleep and timed-wait lists and, when the run queue has work, charges the running thread's timeslice. If the slice expires or idle has work to run, the tick requests preemption. The switch is serviced at an eligible boundary, such as interrupt exit or the outermost preemption-enable, after interrupt and fault nesting and preemption-disable depth have cleared.

The low-level context switch saves and restores the kernel's callee-saved context. An interrupted thread already has a register frame on its kernel stack and eventually resumes through the interrupt return path. A syscall can switch through an explicit yield, wait, or exit, and its exit boundary can service pending preemption. The scheduler also activates the incoming task's address space as needed and saves or restores user FP state when switching out of or into a user thread. See [Tasks](../Kernel/Tasks.md#scheduling-and-address-spaces).

### Wait queues and execution context

Object signal waits, thread joins, and contended kernel mutexes use wait queues. Sleep uses a separate deadline list. The timer also checks deadlines for timed wait-queue entries. A mutex checks its acquisition predicate under the wait-queue lock to close the race between failed acquisition, enqueue, and unlock.

Per-core execution context tracks IRQ, fault, and syscall nesting and distinguishes preemption control from local IRQ masking. Blocking and voluntary scheduling require preemptible thread context with no locks held. Syscall handlers can block in thread context. See [Kernel Locking](../Kernel/Locking.md).

### Thread exit and stack protection

All kernel threads, including per-core idle threads, the reaper, and the kernel shell, belong to [task zero](Task%20Model.md#the-kernel-as-task-zero). An exiting thread marks itself dead, signals termination, and switches away. The reaper waits for the outgoing switch to finish before reclaiming the stack and other thread resources. The last dead thread of a userspace task triggers [task teardown](../Kernel/Tasks.md#teardown).

The scheduler publishes the incoming kernel stack floor on each switch. Trap entry can detect a stack pointer below that floor and divert to an emergency stack to report the overflow. This software check is independent of page protection.

### Observability

The kernel shell exposes per-thread CPU accounting, scheduling counts, migrations, runnable-to-running latency, and a bounded event trace. Trace detail is pulled on demand, and lifecycle logging can be enabled separately.

## Future extensions

The current scheduler has no priorities, fairness weights, scheduling classes, or user-configurable policy. Per-core run queues are a possible future optimization if profiling shows contention on the shared queue lock.
