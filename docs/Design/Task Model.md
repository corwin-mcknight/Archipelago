# Task Model

> [!info] Partial Implementation
> Task creation, ELF loading, ring-3/U-mode entry, scheduling, teardown, kill, exit status, the TERMINATED signal, and spawn-from-VMO are implemented on x86_64 and riscv64.
> Region delegation, the userspace loader, and the broader syscall surface remain planned.

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

## Thread Ownership
Every thread has one non-null parent task for its entire lifetime, including boot and idle threads, which belong to the kernel task.
The thread holds an immutable owning reference to its task; it cannot be reparented or added to another task's thread list.
The reaper removes dead threads from their parent's list, breaking the task/thread ownership cycle.

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

The kernel finds a userspace program called init -- the [[Service Coordination|coordinator]] -- and launches it.
The coordinator then spawns every other task, so the kernel launches exactly one.
There is no special bootstrap mode -- the same code path used for the first launch is used for every launch.

### ELF Loading
The kernel has one built-in binary format loader: ELF.
It parses the ELF binary, creates VMOs for each loadable segment, maps them into the task's address space, and sets the thread's entry point.

The built-in loader is scaffolding for everything except the boot path.
The end state is that the [[Service Coordination|coordinator]] builds executables in memory itself -- ELF, Mach-O, or any other format -- using builder primitives: create an empty task, map VMO ranges into it through region delegation, start a thread at an entry point.
The kernel then parses ELF only to load the coordinator, and does not need to understand every executable format.
See [[Service Coordination#The loader trajectory]].

## Service Discovery
The kernel provides no service naming.
A task reaches a named service through the [[Service Coordination|coordinator]] -- its parent -- over the bootstrap channel it was born holding.
The security boundary is at access, not knowledge: the coordinator decides who connects to what, and the rights on the handed-over channel end determine what operations are permitted.

A kernel-side type-to-server mapping arrives later with the [[Object Model#Type Registration|type registry]] and [[Object Transaction Programs|OTP system]]; it will route operations on typed objects, not introductions.

## Termination
A task dies in one of three ways: its last thread exits, it faults fatally, or someone kills it through its task handle.
Kill is the first type-specific handle operation -- it requires a task-typed handle with the right to terminate.

Kill is asynchronous and unrefusable: the task is marked, each of its threads is forced onto the exit path at its next kernel boundary, and threads blocked in the kernel are woken to exit rather than resume.
The task never executes another user instruction after the mark is observed.

Every death converges on the same teardown, and death is observable two ways:
- The task object asserts a `TERMINATED` signal, so any holder of the task handle can wait on it or bind it to a [[IPC Primitives#Ports|port]].
- Closing the handle table hangs up the task's channels, so peers see `PEER_CLOSED` without holding a task handle.

Exit carries a status: the exit syscall takes one, the C runtime forwards main's return value, and a kill stores a killed marker instead.
The status is recorded on the task object and readable through the task handle after `TERMINATED` asserts.

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
