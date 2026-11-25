# Syscall Interface

## Current ABI

The syscall interface is the boundary between userspace and the kernel. The installed [ABI header](../../src/sys/kernel/includes/abi/syscall.h) defines syscall numbers, arguments, rights, and results.

### Entry and dispatch

x86_64 enters through [`SYSCALL`](../../src/sys/kernel/x86_64/syscall_entry.s) and returns via `SYSRET`. RISC-V enters through [`ecall`](../../src/sys/kernel/riscv64/trap.cpp) and returns via `sret`. Both enter the shared [dispatcher](../../src/sys/kernel/syscalls/dispatch.cpp) on the calling thread's kernel stack with local interrupts masked. Entry preserves six argument slots. Current operations use at most five.

A single switch invokes operation handlers in the [syscalls directory](../../src/sys/kernel/syscalls). The dispatcher pins the calling thread, whose task supplies the handle table and address space. Typed operations use `HandleTable::get<T>()` to check handle validity, type, and rights in that order. Generic object operations use `verify()`. Handle mutations validate inside the table operation itself. An acquired object reference remains valid after unlocking the table, including across a concurrent handle close.

Every handler return reaches the same termination check, including unknown syscall numbers. The dispatcher releases its thread reference before a terminating syscall switches away, because thread exit abandons the syscall stack without unwinding it.

### Buffered arguments

Payload buffers are offsets and lengths into the calling thread's pinned IPC buffer. The [checked range implementation](../../src/sys/kernel/task/ipc_range.cpp) rejects invalid buffers and out-of-bounds or overflowing ranges, then copies across backing frames in bounded page chunks. An empty range at the end of a valid buffer is allowed and touches no frames. A range borrows the backing for the duration of the operation.

Socket handlers return the bytes already transferred when a later chunk fails or transfers only partially. Channel and socket creation share endpoint installation and rollback. Handle IDs are copied out only after both inserts succeed.

### Scheduling and IPC

Yield, sleep, exit, thread exit, debug write, and creation of channels, sockets, ports, and VMOs do not require a handle lookup. Task construction and worker startup require their respective factory capabilities. Userspace finds named services through the [coordinator](Service%20Coordination.md) over its bootstrap channel.

Channel sends fail immediately when the peer queue is full, and receives return immediately when it is empty. Socket reads return available bytes, and writes may be short when the peer buffer has limited room. The caller decides whether to retry, drop, or explicitly wait for readiness. Object and port wait are blocking operations that can take a timeout. See [IPC Primitives](IPC%20Primitives.md) for readiness and backpressure.

### Input and Output

Userspace can write debugging output through `SYS_WRITE` and exchange byte streams through socket handles. The debug write emits bytes from the caller's IPC buffer and has no console object or handle behind it. The kernel ABI has no dedicated standard-stream operations. The userspace file service also uses IPC. See [File Service](File%20Service.md).

## Planned extensions

[Standard Streams](Standard%20Streams.md) describes planned spawner-endowed sockets for stdin, stdout, and stderr, with a userspace console server behind some of those handles. The socket and handle-rights primitives are implemented. The stream endowment and console wiring are planned.

Uniform server routing and [Object Transaction Programs](Object%20Transaction%20Programs.md) remain part of the planned [three-path dispatch](Object%20Model.md#three-path-dispatch) design.
