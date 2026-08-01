# Syscall Interface

> [!info] Design
> This feature is not yet implemented. This page describes the planned design.

The syscall interface is the boundary between userspace and the kernel.
All userspace operations on kernel objects pass through this single entry point.

## Entry Point
There is one syscall entry point.
It uses the x86_64 `SYSCALL`/`SYSRET` instructions for the user-to-kernel-to-user transition.
Arguments are passed in registers -- there is no stack-based parameter passing.

## Dispatch
Every syscall that operates on a handle follows the [[Object Model#Three-Path Dispatch|three-path dispatch pipeline]]:

1. **Handle lookup** -- resolve the handle ID in the calling task's [[Handle Table]], validate the generation counter
2. **Rights check** -- verify the handle carries sufficient rights for the requested operation
3. **Operation dispatch** -- execute the [[Object Transaction Programs|transaction program]] if one is attached, otherwise forward to the owning server via [[IPC Primitives|IPC]]

This is the same pipeline regardless of object type.
The kernel does not have per-type syscall handlers -- the object model provides uniform dispatch.

## Non-Handle Syscalls
A small number of syscalls do not operate on handles:
- Thread yield and exit
- Service discovery queries
- System information queries
- Debug output

These bypass the three-path pipeline because there is no handle to look up.

Debug output is a kernel debugging convenience, not the system's I/O mechanism, and there is deliberately no console object or console handle behind it. Real input and output are an open design question -- see below -- and nothing about the debug write should be read as answering it.

## Input and Output
Undesigned. This system has no files, and will not grow "standard input" and "standard output" as file-shaped things -- that is a UNIX answer to a question this object model asks differently.

What the existing pieces imply is that a program producing output holds a [[IPC Primitives#Channels|channel]] to a server that owns the device, obtained through service discovery rather than inherited by position at startup, and that bulk data moves by handle rather than by copy. What that means for the ordinary case -- how a program with nothing but its own task and thread handles reaches a place to write to, and what a device server's interface looks like -- is not settled.

## Kernel Non-Blocking Guarantee
The kernel never blocks on behalf of a caller during IPC.
If a [[IPC Primitives#Channels|channel]] queue is full, the send fails immediately.
The calling task decides how to handle backpressure -- wait on the `WRITABLE` signal, retry, or drop.
See [[IPC Primitives#Signals]].

## Relationship to Other Subsystems
- [[Object Model#Three-Path Dispatch]] -- the dispatch pipeline that every handle syscall follows
- [[Handle Table]] -- handle resolution is the first step of dispatch
- [[Object Transaction Programs]] -- the programmable layer between rights check and IPC
- [[IPC Primitives]] -- the fallback path when an operation reaches dispatch
- [[Task Model]] -- syscalls are the boundary between task code and kernel code
- [[Scheduling]] -- syscall entry and exit are context switch points
