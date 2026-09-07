# Tasks
Archipelago tasks are kernel objects that bind a handle table, a set of threads, and -- for userspace tasks -- a virtual address space into one authority boundary. The kernel itself is task zero.

## Lifecycle
A task begins in `NEW`, enters `RUNNING` when its first thread is queued, and becomes `TERMINATED` after the reaper removes its last thread. Task zero is created directly in `RUNNING`, owns no userspace address space, and never terminates.

The kernel starts an already prepared address space: it wires the bootstrap channel, queues the bootstrap message, and queues the first thread at the supplied entry and stack addresses. A separate boot-only path creates init from its fixed-layout non-ELF image, once.

## Constructing a task
A TaskFactory capability authorizes a loading thread to create one dormant address-space container. MAP copies page-aligned source VMO ranges into private backing with R, RW, or RX permissions. Neither the loader nor a sibling task can retain a writable alias to executable backing. START checks entry and stack mappings, consumes the container, and returns task and parent-bootstrap handles. ABORT discards the container.

The container belongs to the calling thread; other threads cannot access it. The reaper releases unfinished construction before removing a dead thread from its task, even if a handle retains the Thread object. Successful start makes the child's lifetime independent of the loading thread. General region delegation is not yet exposed.

## Loading an executable
The `lib/elf` userspace library accepts static `ET_EXEC` images for the running architecture. Its pure parser validates bounds, architecture, permissions, and entry coverage. It rejects dynamic executables, unreadable or RWX segments, unaligned addresses, oversized memory extents, and truncated input. The loader prepares zero-filled VMOs for segment tails and `.bss`, supplies a writable non-executable stack, and invokes the construction syscalls.

Init uses the library only to bootstrap `sys/elf_loader`, then transfers construction authority to that service. Each request runs in a fresh worker thread with its own IPC buffer. The loader returns task and parent-bootstrap handles to init, and applications receive no construction capability. Routine loading failures are returned to init; an unexpected worker fault terminates the service to reclaim task-owned temporary resources.

Bootstrap handles are ordered: self task, initial thread, task-bound ThreadFactory, and (for boot init only) TaskFactory.

## Scheduling and address spaces
Every thread has an immutable, non-null reference to its parent task. Boot and idle threads belong to task zero; a thread cannot be added to another task's thread list. Spawned threads also record their kernel stack top. On a context switch, the scheduler activates the incoming task's address space, or the kernel address space for task zero, when it differs from the active space.

The scheduler also publishes the incoming kernel stack for privilege transitions. x86_64 writes TSS `rsp0` and the SYSCALL entry stack. riscv64 reconstructs the stack top in `sscratch` whenever a trap returns to U-mode.

User FP/SIMD state is carried per thread and switched eagerly, but only for user threads: the kernel itself is built without FP or vector instructions, so whatever user code left in those registers survives every kernel entry -- and every kernel-thread stretch -- untouched, and the scheduler saves and restores it only when a switch leaves or enters a thread whose task has an address space. On x86_64 this is the FXSAVE area, giving user programs the standard ABI including SSE2. On riscv64 it is the f-register file plus fcsr, giving user programs the lp64d ABI; sstatus.FS is switched on per hart when the trap vector is installed and never turns Off, so FP execution needs no per-trap management.

## Syscalls
The initial syscall surface is deliberately small:

- `exit` (`0`) records task exit status and terminates the calling thread.
- `thread_start` (`29`) requires a ThreadFactory capability with WRITE, bound to the caller's task. Every application receives one at bootstrap and can create its own threads without TaskFactory authority.
- `thread_exit` (`30`) terminates only the calling thread without recording a task exit status.
- `yield` (`1`) cooperatively rotates the scheduler run queue.
- `sleep` (`2`) blocks the calling thread for at least `arg0` kernel ticks.
- `write` (`3`) emits a range of the calling thread's IPC buffer, given as an offset and a length. It returns the byte count written, or a negative error code.
- `handle_close` (`4`) closes the handle in `arg0`.
- `handle_duplicate` (`5`) creates a second handle to `arg0`'s object carrying its rights ANDed with `arg1`; the source handle must carry the duplicate right.
- `obj_info` (`6`) returns the handle's object type id in the low 32 bits and its rights in the high 32.

The syscall number names the operation; a handle argument names the object it acts on.

## Handle operations
One syscall switch calls ordinary handlers. Typed operations use `get<T>()` to check handle validity, type, and rights under one table lock; generic object operations use `verify()`. Close, duplicate, and restriction perform their own validation within the table operation. Acquired references pin objects after unlocking and across concurrent handle closes.

The dispatcher pins the calling thread once and passes it to handlers that need the IPC buffer or task. Every return crosses the same termination check, including unknown syscall numbers. All references and locks are released before exiting a thread, because exit abandons the stack without unwinding.

A handle crossing the boundary is a uint64: table slot index in the low 32 bits, generation in the high 32. A closed slot's generation moves, so a stale handle fails the lookup rather than reaching whatever now occupies the slot.

The initial thread's table is created with exactly one entry, promised by the ABI as first-generation slot 0: one end of its bootstrap channel. The other end belongs to the task's creator -- the kernel for init, and ultimately init for loaded applications. Only the kernel-parented endpoint is held as Task::mailbox. The first message queued on the channel, before the thread can run, is the bootstrap message: an empty payload carrying a handle to the task itself (read and write rights), a handle to its initial thread (read and wait), and any further handles the creator endowed the task with. Everything after that first message is ordinary parent-to-task mail. Neither self-handle carries the duplicate right, which makes the rights-rejection path reachable from the first program.

x86_64 enters through SYSCALL/SYSRET. riscv64 enters through `ecall` and returns through `sret`. Both call the shared dispatcher with interrupts disabled on the calling thread's kernel stack. Six argument registers are carried -- as many as either architecture's calling convention provides, so the entry assembly never needs widening again -- though current operations read at most five.

## The IPC buffer
No pointer crosses the syscall boundary. Every thread with an address space is given a buffer at creation -- a committed anonymous VMO mapped into its task, sized when the thread is spawned and capped by a system constant -- and that buffer is the only memory a syscall ever reads from its caller. A syscall argument naming data is an offset into it.

The kernel resolves the buffer's frames once, when the thread is created, and keeps their physmap addresses. So a buffered syscall performs no page-table walk, takes no VMM lock, and cannot fault: it checks a range against a size it already knows and reads its own direct map. There is no window in which the mapping can change under the kernel, and no address the caller can name that the kernel has to be talked into trusting. A task that unmaps its own buffer only blinds itself -- the kernel holds its own reference, so the frames stay alive and unchanged.

The checked-range API rejects invalid buffers, out-of-bounds offsets, and overflowing lengths. It yields bounded page chunks and copies across noncontiguous frames. Empty ranges at the end of a valid buffer are allowed and touch no frames. A range borrows its buffer and is used while the dispatcher pins the calling thread.

A thread learns where its buffer is from two registers set at entry, rather than from a constant in the ABI, which leaves the address free to move when user-space layout randomisation arrives.

## Teardown
The reaper performs user-task teardown after removing the last dead thread:

1. Release the dead thread's unfinished construction, IPC buffer, and kernel stack.
2. Clear the final task's handle table and drop its mailbox, releasing queued bootstrap escrow.
3. Switch to the kernel address space if necessary, then destroy the user address space.
4. Remove the task from the global registry and close task zero's owner handle.
5. Publish `TERMINATED` and its completion signal.

Task zero is exempt from task teardown. Task kill closes further thread creation before taking its thread snapshot, preventing a newly added worker from escaping the kill. Unresolved user faults terminate the faulting user thread and record the task's fault status; they do not enter the kernel crash path.
