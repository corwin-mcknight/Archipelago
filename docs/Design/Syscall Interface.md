# Syscall Interface

The syscall interface is the boundary between userspace and the kernel.
The installed ABI lives in `src/sys/kernel/includes/abi/syscall.h`.

## Entry Point
Architecture entry code passes the syscall number and register arguments to one shared dispatcher.
It pins the current thread once; the thread's parent task supplies the handle table and address space.
Every handler return crosses a common exit boundary that checks for termination, including unknown syscall numbers.
References and locks are released before thread exit abandons the syscall stack.

## Dispatch
A single switch routes syscall numbers to ordinary handlers.
Typed operations call `HandleTable::get<T>()`, checking handle validity, type, and rights in that order.
Generic object operations use `verify()`; handle mutations validate within the table operation itself.
Acquired object references remain valid across concurrent handle closes, and operations run after the table lock is released.

Uniform server routing and [[Object Transaction Programs]] remain part of the planned [[Object Model#Three-Path Dispatch|three-path dispatch]] design.

## Buffered Arguments
Memory arguments are offsets and lengths into the calling thread's pinned IPC buffer.
A checked range rejects invalid buffers, out-of-bounds offsets, and overflowing lengths before an operation starts.
Ranges yield bounded, contiguous page chunks and provide copies across noncontiguous backing frames.
An empty range at the end of a valid buffer is allowed; no frame is accessed for an empty copy.
Ranges borrow the buffer and must not outlive or mutate its backing description.

Socket handlers preserve partial progress: a short transfer or a later error returns the bytes already transferred.
Channel and socket creation share endpoint installation and rollback; handle IDs are copied out only after both inserts succeed.

## Non-Handle Syscalls
A small number of syscalls do not operate on handles:
- Thread yield, sleep, and exit
- Object creation
- Debug output

These operations do not require a handle lookup.
Service discovery is deliberately not in this list: reaching a named service is a conversation with the [[Service Coordination|coordinator]] over the bootstrap channel, not a syscall.

Debug output is a kernel debugging convenience, not the system's I/O mechanism, and there is deliberately no console object or console handle behind it. Real input and output are an open design question -- see below -- and nothing about the debug write should be read as answering it.

## Input and Output
This system has no files, and stdio is not file-shaped: no descriptor numbers, no paths, no open.
A program's ordinary input and output are [[IPC Primitives#Sockets|socket]] ends endowed at spawn -- see [[Standard Streams]].
The capability is inherited, but what it names is not a file; the writer holds an object it can write, wait on, and pass along like any other, and bulk data still moves by handle rather than by copy.
There are no stdio syscalls: writing to output is the ordinary socket write on an ordinary handle, and the console is a userspace server behind some of those handles, not a kernel object.

## Kernel Non-Blocking Guarantee
The kernel never blocks on behalf of a caller during IPC.
If a [[IPC Primitives#Channels|channel]] queue is full, the send fails immediately.
The calling task decides how to handle backpressure -- wait on the `WRITABLE` signal, retry, or drop.
See [[IPC Primitives#Signals]].

## Relationship to Other Subsystems
- [[Object Model#Three-Path Dispatch]] -- planned uniform server routing
- [[Handle Table]] -- handle resolution is the first step of dispatch
- [[Object Transaction Programs]] -- the programmable layer between rights check and IPC
- [[IPC Primitives]] -- channels, sockets, signals, and ports
- [[Task Model]] -- syscalls are the boundary between task code and kernel code
- [[Scheduling]] -- syscall entry and exit are context switch points
