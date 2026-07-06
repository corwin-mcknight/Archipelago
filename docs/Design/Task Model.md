# Task Model

> [!info] Design
> This feature is not yet implemented. This page describes the planned design.

A task is the unit of isolation in Archipelago.
It is deliberately not called a "process" -- there is no UNIX lineage here.
No PID namespace, no fork/exec, no parent-child trees, no signal delivery model.
A task is an authority boundary, nothing more.

## Structure
A task contains three things:
- A [[Handle Table]] -- the task's set of capabilities
- A virtual address space -- the task's memory mappings
- A collection of threads -- the task's execution contexts

The virtual address space is part of the task, not a separate kernel object.
There is no address space handle.
A task can only map [[Memory Subsystem#Virtual Memory Manager|VMOs]] into its own address space -- there is no ambient mechanism to map into another task's memory.
If task A wants task B to access shared memory, it sends a VMO handle through a [[IPC Primitives#Channels|channel]] and task B maps it itself.
The one planned exception is region delegation: a task may hand out a handle to a region of its own address space, granting the holder the right to map into that interval.
That is deliberate delegation through a capability, not ambient authority; it is how a loader populates a new task's address space, and region handles arrive with the task and IPC milestone.

## Tasks as Kernel Objects
Tasks are [[Object Model|kernel objects]] like any other.
They are accessed through handles, follow the same rights model, and go through the same [[Object Model#Three-Path Dispatch|dispatch pipeline]].
A task handle is not granted automatically -- you hold one only if someone gave it to you.

The kernel holds a handle to every task.
This makes the kernel the owner-of-last-resort for task lifecycle.
A terminated task is not truly dead until the kernel closes its handle, which triggers the destructor chain.

## First Thread Authority
When a task's first thread begins execution, it receives two handles:
- A handle to its own task
- A handle to its own thread

These handles are the task's minimum authority floor.
All further authority comes from handles passed in at creation or acquired through [[IPC Primitives|IPC]].

## The Kernel as Task Zero
The kernel itself is task zero.
It must be schedulable (idle thread), so it exists as a task with its own [[Handle Table]].

The kernel does not route its own internal operations through its handle table.
Internally, it uses direct references.
The handle table serves two practical purposes:
- **Staging** -- newly created handles are constructed in the kernel's table, then transferred to the target task.
  This is simpler than constructing handles directly in another task's table.
- **Lifecycle ownership** -- the kernel holds handles to all tasks.
  A task's destruction is ordered by the kernel closing its handle after cleanup.

## Bootstrap
The kernel follows the normal task creation path to launch the first userspace program.
Because the kernel is task zero with full rights, it can do everything any parent task could do:
create a new task object, populate its handle table, map memory, and start a thread.

The kernel finds a userspace program called init and launches it.
There is no special bootstrap mode -- the same code path used for the first launch is used for every launch.

### ELF Loading
The kernel has one built-in binary format loader: ELF.
It parses the ELF binary, creates VMOs for each loadable segment, maps them into the task's address space, and sets the thread's entry point.

Other binary formats are handled entirely in userspace.
A shim loader -- for example, a Mach-O loader -- would create a task within itself, map the foreign binary's segments, start the new task's thread, then clean up its own resources.
The kernel does not need to understand every executable format.

## Service Discovery
Discovery of available services is open and queryable.
Any task can see what services exist on the system.
The security boundary is at access, not knowledge.
Opening a channel to a service requires the right capabilities, and the rights on the resulting handle determine what operations are permitted.

The kernel brokers service introductions because it already tracks the type-to-server mapping through the [[Object Model#Type Registration|type registry]].
This is a natural consequence of the [[Object Transaction Programs|OTP system]] -- the kernel must know about registered types to execute programs on their behalf.

## Teardown
A task being torn down means all its threads are already dead.
The kernel's reaper thread handles cleanup asynchronously:

1. Scan the task's handle table and close every handle
2. Closing handles may trigger further object destruction, signal delivery, and resource reclamation
3. Tear down the task's address space
4. Destroy the task object itself (the kernel closes its handle)

If anything fails during teardown, the system does not crash.
A zombie task is acceptable -- a kernel panic is not.
Availability always takes priority over purity.

## Relationship to Other Subsystems
- [[Handle Table]] -- each task owns one; the kernel's table stages transfers
- [[Memory Subsystem]] -- the VMM manages the task's virtual address space
- [[Scheduling]] -- threads within a task are scheduled by the kernel
- [[Syscall Interface]] -- the boundary between task code and kernel code
- [[IPC Primitives]] -- tasks communicate by passing messages and handles through channels
- [[Server Lifecycle]] -- servers are tasks that register object types
