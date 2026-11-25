# Task Model

A task is the unit of isolation in Archipelago.
It is deliberately not called a "process" -- there is no UNIX lineage here.
No PID namespace, no fork/exec, no parent-child trees, no signal delivery model.
The task defines the boundary for handles, address-space mappings, and thread ownership.

## Current behavior

The task model is implemented on x86_64 and riscv64.

### Structure
A task contains three things:
- A [[Handle Table]] -- the task's set of capabilities
- A virtual address space -- the task's memory mappings
- A collection of threads -- the task's execution contexts

The virtual address space is part of the task, not a separate kernel object.
There is no address space handle.
A task can only map [[Memory Subsystem#Virtual Memory Manager|VMOs]] into its own address space -- there is no ambient mechanism to map into another task's memory.
If task A wants task B to access shared memory, it sends a VMO handle through a [[IPC Primitives#Channels|channel]] and task B maps it itself.
A loader with a TaskFactory capability can also populate a dormant address space through the task construction interface described under [Bootstrap](#bootstrap).

### Tasks as Kernel Objects
Tasks are [[Object Model|kernel objects]] like any other.
Userspace accesses them through typed, rights-checked [syscalls](Syscall%20Interface.md#entry-and-dispatch).
A task handle is not granted automatically -- you hold one only if someone gave it to you.

Task zero holds an owner handle to each userspace task until teardown. Other holders can retain the terminated task object to observe its completion and read its status.

### Thread Ownership
Every thread has one non-null parent task for its entire lifetime, including boot and idle threads, which belong to the kernel task.
The thread holds an immutable owning reference to its task; it cannot be reparented or added to another task's thread list.
The reaper removes dead threads from their parent's list, breaking the task/thread ownership cycle.

### First Thread Authority
The new task starts with a bootstrap channel handle in slot zero of its shared handle table. The first queued message carries its self task handle, initial thread handle, and task-bound ThreadFactory. Boot init also receives the initial TaskFactory capability. A worker started through ThreadFactory shares the task's handles and mappings and gets its own IPC buffer. See [Tasks](../Kernel/Tasks.md#bootstrap-handles) for handle order and endpoint ownership.

### The Kernel as Task Zero
The kernel itself is task zero.
It owns the per-core idle threads and other kernel threads, and has its own [[Handle Table]]. The [scheduler](Scheduling.md) runs its threads in the kernel address space.

The kernel does not route its own internal operations through its handle table.
Internally, it uses direct references.
The handle table serves two practical purposes:
- **Staging** -- newly created handles are constructed in the kernel's table, then transferred to the target task.
  This is simpler than constructing handles directly in another task's table.
- **Lifecycle ownership** means holding each userspace task until its resources have been torn down.

### Bootstrap
The kernel loads init exactly once, from a minimal non-ELF boot image. It installs the fixed regions, stack, and bootstrap channel with the initial handles before making init runnable. No syscall exposes the boot loader.

Init uses a userspace ELF library to start the dedicated ELF loader service, then transfers construction authority to it. Subsequent program images are sent to that service. It returns the child's task handle and parent bootstrap endpoint to init, which remains the service coordinator for the child.

#### Executable loading
Executable-format parsing belongs entirely to userspace. A loader worker uses its TaskFactory capability to create a thread-owned dormant address-space container, populate private mappings, and start the initial thread at an entry address with a prepared stack. The kernel validates addresses and permissions; it never interprets executable bytes.

Aborting construction or exiting its owning worker releases the unfinished address space. Successful start consumes construction and gives the child an independent lifetime. Applications receive no TaskFactory capability by default. See [[Executable Loading]] for the boot contract, authority, W^X, and failure rules.

### Service Discovery
The kernel provides no service naming.
A task reaches a named service through the [[Service Coordination|coordinator]] over the bootstrap channel it was born holding.
The security boundary is at access, not knowledge: the coordinator decides who connects to what, and the rights on the handed-over channel end determine what operations are permitted.

### Termination

The last thread's exit triggers task teardown. A normal `exit` records a task status and exits the caller. A `thread_exit` exits the caller without recording a task status. An unresolved user fault records a fault cause and exits the faulting thread. Other threads can continue to run.

Task kill requires a task handle with WRITE rights. A successful kill closes thread creation, marks remaining live threads, and wakes threads parked in sleep or a wait queue. A marked thread exits at the syscall boundary or through the scheduler's preemption path if it keeps running in userspace without a syscall. See [[Accepted Constraints]] for the behavior of killed waiters and threads contending on kernel mutexes.

The reaper waits for dead threads to switch off their cores before reclaiming their resources. After the final thread is reaped, it clears the task's handles and address space, removes the task from the registry, and closes the kernel's owner handle. It then publishes `TERMINATED`, which can be waited on directly or bound to a [[IPC Primitives#Ports|port]]. A task handle with READ rights can then retrieve the recorded exit cause and status. See [Tasks](../Kernel/Tasks.md#teardown) for the cleanup order.

## Planned extensions

General region delegation would let a task grant a capability to map into a specific interval of its address space. The planned [[Object Model#Type Registration|type registry]] and [[Object Transaction Programs|OTP system]] would route typed object operations. Service discovery continues to use the coordinator protocol.

## Relationship to Other Subsystems
- [[Handle Table]] -- each task owns one; the kernel's table stages transfers
- [[Memory Subsystem]] -- the VMM manages the task's virtual address space
- [[Scheduling]] -- threads within a task are scheduled by the kernel
- [[Syscall Interface]] -- the boundary between task code and kernel code
- [[IPC Primitives]] -- tasks communicate by passing messages and handles through channels
