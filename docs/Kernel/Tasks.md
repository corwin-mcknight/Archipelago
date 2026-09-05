# Tasks
Archipelago tasks are kernel objects that bind a handle table, a set of threads, and -- for userspace tasks -- a virtual address space into one authority boundary. The kernel itself is task zero.

## Lifecycle
A task begins in `NEW`, enters `RUNNING` when its first thread is queued, and becomes `TERMINATED` after the reaper removes its last thread. Task zero is created directly in `RUNNING`, owns no userspace address space, and never terminates.

The creation path takes an ELF image as a byte span and loads it through the kernel's ELF loader: each loadable segment becomes an anonymous VMO mapped at its own address with its own protection, the entry point comes from the image, and a demand-paged `USER|READ|WRITE` stack is mapped at a fixed address the kernel chooses. It then wires the bootstrap channel, queues the bootstrap message, and queues the first thread. Where the image came from is the caller's business -- today the boot protocol's `init` module, later a filesystem.

## Loading a binary
The loader accepts static `ET_EXEC` images for the running architecture and nothing else. It is split so that the part doing arithmetic over untrusted header fields is separable from the part that touches memory: parsing validates the header and collects loadable segments without allocating, touching global state, or knowing about the VMM, and mapping turns that result into VMOs and bindings.

Rejections are named rather than generic, because the parser cannot log and a refused binary is otherwise indistinguishable from a broken one. Dynamically linked images are refused outright rather than loaded without their interpreter, segments that are both writable and executable are refused, and segments must begin on a page boundary. Memory beyond a segment's file contents needs no special handling: anonymous VMOs zero-fill, which covers both `.bss` and the tail of a partially filled page.

## Scheduling and address spaces
Every thread has an immutable, non-null reference to its parent task. Boot and idle threads belong to task zero; a thread cannot be added to another task's thread list. Spawned threads also record their kernel stack top. On a context switch, the scheduler activates the incoming task's address space, or the kernel address space for task zero, when it differs from the active space.

The scheduler also publishes the incoming kernel stack for privilege transitions. x86_64 writes TSS `rsp0` and the SYSCALL entry stack. riscv64 reconstructs the stack top in `sscratch` whenever a trap returns to U-mode.

User FP/SIMD state is carried per thread and switched eagerly, but only for user threads: the kernel itself is built without FP or vector instructions, so whatever user code left in those registers survives every kernel entry -- and every kernel-thread stretch -- untouched, and the scheduler saves and restores it only when a switch leaves or enters a thread whose task has an address space. On x86_64 this is the FXSAVE area, giving user programs the standard ABI including SSE2. On riscv64 it is the f-register file plus fcsr, giving user programs the lp64d ABI; sstatus.FS is switched on per hart when the trap vector is installed and never turns Off, so FP execution needs no per-trap management.

## Syscalls
The initial syscall surface is deliberately small:

- `exit` (`0`) terminates the calling thread and does not return.
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

The initial thread's table is created with exactly one entry, promised by the ABI as first-generation slot 0: one end of its bootstrap channel. The other end belongs to the task's creator -- the kernel today, held as the task's mailbox for the task's whole life. The first message queued on the channel, before the thread can run, is the bootstrap message: an empty payload carrying a handle to the task itself (read and write rights), a handle to its initial thread (read and wait), and any further handles the creator endowed the task with. Everything after that first message is ordinary parent-to-task mail. Neither self-handle carries the duplicate right, which makes the rights-rejection path reachable from the first program.

x86_64 enters through SYSCALL/SYSRET. riscv64 enters through `ecall` and returns through `sret`. Both call the shared dispatcher with interrupts disabled on the calling thread's kernel stack. Six argument registers are carried -- as many as either architecture's calling convention provides, so the entry assembly never needs widening again -- though current operations read at most five.

## The IPC buffer
No pointer crosses the syscall boundary. Every thread with an address space is given a buffer at creation -- a committed anonymous VMO mapped into its task, sized when the thread is spawned and capped by a system constant -- and that buffer is the only memory a syscall ever reads from its caller. A syscall argument naming data is an offset into it.

The kernel resolves the buffer's frames once, when the thread is created, and keeps their physmap addresses. So a buffered syscall performs no page-table walk, takes no VMM lock, and cannot fault: it checks a range against a size it already knows and reads its own direct map. There is no window in which the mapping can change under the kernel, and no address the caller can name that the kernel has to be talked into trusting. A task that unmaps its own buffer only blinds itself -- the kernel holds its own reference, so the frames stay alive and unchanged.

The checked-range API rejects invalid buffers, out-of-bounds offsets, and overflowing lengths. It yields bounded page chunks and copies across noncontiguous frames. Empty ranges at the end of a valid buffer are allowed and touch no frames. A range borrows its buffer and is used while the dispatcher pins the calling thread.

A thread learns where its buffer is from two registers set at entry, rather than from a constant in the ABI, which leaves the address free to move when user-space layout randomisation arrives.

## Teardown
The reaper performs user-task teardown after removing the last dead thread:

1. Mark the task `TERMINATED`.
2. Clear its handle table and drop its mailbox, destroying the bootstrap channel and closing any handles still escrowed on it -- including the task's own bootstrap self-reference.
3. Switch to the kernel address space if necessary, then destroy the user address space.
4. Remove the task from the global registry.
5. Close task zero's owner handle.

Task zero is exempt from this path. Unresolved user faults still enter the kernel crash path; task-local fault termination requires the future kill machinery.
