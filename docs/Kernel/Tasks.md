# Tasks
Archipelago tasks are kernel objects that bind a handle table, a set of threads, and -- for userspace tasks -- a virtual address space into one authority boundary. The kernel itself is task zero.

## Lifecycle
A task begins in `NEW`, enters `RUNNING` during launch before its first thread is queued, and becomes `TERMINATED` when the reaper finishes teardown. Task zero is created directly in `RUNNING`, owns no userspace address space, and never terminates.

The kernel starts an already prepared address space: it wires the bootstrap channel, queues the bootstrap message, and queues the first thread at the supplied entry and stack addresses. A separate boot-only path creates init from its fixed-layout non-ELF image, once.

## Constructing a task
A TaskFactory capability authorizes a loading thread to create one dormant address-space container. MAP copies page-aligned source VMO ranges into private backing with R, RW, or RX permissions. Neither the loader nor a sibling task can retain a writable alias to executable backing. START checks entry and stack mappings, consumes the container, and returns task and parent-bootstrap handles. ABORT discards the container.

The container belongs to the calling thread; other threads cannot access it. The reaper releases unfinished construction before removing a dead thread from its task, even if a handle retains the Thread object. Successful start makes the child independent of the loading thread. See [planned region delegation](../Design/Task%20Model.md#planned-extensions).

## Loading an executable
The `lib/elf` userspace library accepts static `ET_EXEC` images for the running architecture. Its pure parser validates bounds, architecture, permissions, and entry coverage. It rejects dynamic executables, unreadable or RWX segments, unaligned addresses, oversized memory extents, and truncated input. The loader prepares zero-filled VMOs for segment tails and `.bss`, supplies a writable non-executable stack, and invokes the construction syscalls.

Init uses the library only to bootstrap `sys/elf_loader`, then transfers construction authority to that service. Each request runs in a fresh worker thread with its own IPC buffer. The loader returns task and parent-bootstrap handles to init, and applications receive no construction capability. Routine loading failures are returned to init; an unexpected worker fault terminates the service to reclaim task-owned temporary resources.

## Bootstrap handles

The [launch path](../../src/sys/kernel/task/user_task.cpp) starts the task's shared handle table with a single entry, the child end of its bootstrap channel in first-generation slot zero. Task zero retains the peer end for boot init. For applications, the loader returns the peer end to the coordinator.

Before the initial thread can run, the kernel queues an empty bootstrap message carrying the self task handle (READ and WRITE), the initial thread handle (READ and WAIT), and the task-bound ThreadFactory (WRITE and DUPLICATE), in that order. Boot init receives TaskFactory (WRITE and DUPLICATE) as the fourth handle. Later messages on that channel carry coordinator traffic.

## Scheduling and address spaces
Every thread has an immutable, non-null reference to its parent task. Boot and idle threads belong to task zero; a thread cannot be added to another task's thread list. Spawned threads also record their kernel stack top. On a context switch, the scheduler activates the incoming task's address space, or the kernel address space for task zero, when it differs from the active space.

Scheduling cores share one FIFO run queue. A thread may migrate between cores after its outgoing switch completes. On x86_64, switching to a thread with a recorded kernel stack top updates TSS `rsp0` and the SYSCALL entry stack. On riscv64, the U-mode return path installs the kernel stack top in `sscratch`. See [Scheduling](../Design/Scheduling.md).

The scheduler eagerly saves and restores FP state when switching out of or into user threads. Kernel code leaves the user registers untouched between switches. x86_64 uses FXSAVE/FXRSTOR, including SSE2 state. riscv64 saves the floating-point register file and `fcsr` for the LP64D ABI. Each hart enables floating-point execution when it installs its trap vector.

## Syscalls
[Syscall Interface](../Design/Syscall%20Interface.md) covers architecture entry, the shared dispatcher, and handle lookup. The [ABI header](../../src/sys/kernel/includes/abi/syscall.h) defines the current operations and their numeric values.

`SYS_THREAD_START` requires a WRITE-capable ThreadFactory bound to the calling task. `SYS_THREAD_EXIT` terminates the caller without recording a task exit status. `SYS_EXIT` records a task status and terminates the caller. `SYS_TASK_KILL` and `SYS_TASK_STATUS` operate on task handles with WRITE and READ rights, respectively.

## The IPC buffer
Each user thread gets a dedicated IPC buffer backed by a committed anonymous VMO mapped into its task. The kernel records the direct-map addresses of its frames at creation and holds the backing until thread cleanup. Buffered syscall copies use those pinned frames without walking user page tables, even if the task unmaps its view. The kernel passes the buffer base and size to the thread in entry registers. See [Buffered arguments](../Design/Syscall%20Interface.md#buffered-arguments) for range checking.

## Teardown
The [reaper](../../src/sys/kernel/task/reaper.cpp) waits for the dead thread's core to switch away. It releases unfinished construction, removes the thread from its task, and reclaims its IPC buffer and kernel stack. A retained thread handle can keep the object alive after those resources are reclaimed.

When the last thread of a userspace task is reaped, the kernel clears the task's handle table and drops its mailbox to release queued bootstrap escrow. It switches to the kernel address space if needed and destroys the user address space. It then removes the task from the registry, closes task zero's owner handle, and publishes `TERMINATED` and its completion signal.

Task zero is exempt from task teardown. Task kill closes further thread creation before taking its thread snapshot. An unresolved user fault records the task's fault cause and terminates the faulting thread through the scheduler.
