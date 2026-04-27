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

These bypass the three-path pipeline because there is no handle to look up.

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
