# Tasks
Archipelago tasks are kernel objects that bind a handle table, a set of threads, and -- for userspace tasks -- a virtual address space into one authority boundary. The kernel itself is task zero.

## Lifecycle
A task begins in `NEW`, enters `RUNNING` when its first thread is queued, and becomes `TERMINATED` after the reaper removes its last thread. Task zero is created directly in `RUNNING`, owns no userspace address space, and never terminates.

The current creation path loads a small architecture-specific payload embedded in the kernel. It maps code `USER|READ|EXECUTE`, maps a demand-paged `USER|READ|WRITE` stack, installs kernel and self handles, and queues the first thread. ELF loading will replace the fixed payload and addresses.

## Scheduling and address spaces
Every spawned thread keeps a reference to its owning task and records its kernel stack top. On a context switch, the scheduler activates the incoming user task's address space only when it differs from the active address space. Kernel threads have no private address space and may run in the currently active user space because every user space includes the shared kernel mappings.

The scheduler also publishes the incoming kernel stack for privilege transitions. x86_64 writes TSS `rsp0` and the SYSCALL entry stack. riscv64 reconstructs the stack top in `sscratch` whenever a trap returns to U-mode.

## Syscalls
The initial syscall surface is deliberately small:

- `exit` (`0`) terminates the calling thread and does not return.
- `yield` (`1`) cooperatively rotates the scheduler run queue.
- `sleep` (`2`) blocks the calling thread for at least `arg0` kernel ticks.
- `debug_putc` (`3`) appends one character to a kernel-side line buffer; a newline (or a full buffer) flushes the line to the kernel log at info level.

x86_64 enters through SYSCALL/SYSRET. riscv64 enters through `ecall` and returns through `sret`. Both call the shared dispatcher with interrupts disabled on the calling thread's kernel stack.

## Teardown
The reaper performs user-task teardown after removing the last dead thread:

1. Mark the task `TERMINATED`.
2. Clear its handle table, breaking self-handle reference cycles.
3. Switch to the kernel address space if necessary, then destroy the user address space.
4. Remove the task from the global registry.
5. Close task zero's owner handle.

Task zero is exempt from this path. Unresolved user faults still enter the kernel crash path; task-local fault termination requires the future kill machinery.
