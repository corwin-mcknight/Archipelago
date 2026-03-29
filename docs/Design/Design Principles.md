# Design Principles

These principles guide the kernel's architecture and inform every design decision.
They describe where Archipelago is headed, not necessarily what is built today.

## The Kernel Is Taught
The kernel is simply a safe marshal between userspace and kernelspace. Userspace tells the kernel about how it should operate, and the kernel obeys.  However, the kernel will always prioritize safety.

The kernel can manage buffers, flip signal bits, increment counters, enforce rights. It can even hold object storage. But it doesn't interpret meaning in any way, that's the job of *servers*. 

## Authority isn't execution

When an object type is created, it actually has two properties -- authority, and execution.
In monolithic systems, the kernel has authority, and execution of code. Traditionally, a mutex is defined in the kernel and is accessed with system calls. 

In a microkernel, the server would define the object, has authority, and executes code.

In Archipelago, the server has authority and defines the object, but the kernel has execution in addition to the server. This actually starts to blur the lines between an exokernel (which just provides access to raw hardware) and microkernel.

## Graduated opacity
The kernel generally doesn't understand what objects do, but knows properties about them. For example, it has no concept of operating a socket, but it knows properties of socket and can do some optimization on behalf of userspace.  Effectively, types are not usually fully transparent to the kernel, but they're not opaque either.

This challenges the false dichotomy that microkernels must choose between owning a type completely (monolithic) or being a dumb pipe (pure microkernel).
## Performance through avoidance, not speed
The classic microkernel answer to performance is "make IPC fast enough."
Our answer is "make IPC *avoidable* when the server tells you it's safe to."
This is strictly more powerful.

## Servers adapt to the system, not the other way around
The kernel exposes system properties (page size, architecture, capabilities).
Servers query these properties and choose how to register their types accordingly.
The kernel just handles requests -- it doesn't make smart decisions about allocation strategy or storage layout.

## Fail hard, recover clean
Server crash kills every handle and object of that type.
No partial recovery, no zombie objects, no state reconstruction.
A server crash just makes a lot of those happen at once.
Reliability is the server's problem, not the kernel's.
