# MILESTONES

## Milestone 1 -- User-mode Hello World
An initial user program is loaded and run by the kernel.
It outputs "hello world" to the serial console via a syscall.
The program's ELF binary is loaded by the kernel's built-in ELF loader, mapped with VMOs into the task's virtual address space.
A thread is created and begins execution, context switching through syscalls the task makes.
When the program finishes, the task enters a TERMINATED state.
The system does not crash -- it dumps the user into the kernel shell, from which the program can be re-launched.

### Subsystems Required
Each of these must be implemented to reach this milestone.

#### Virtual Memory Manager
Page table management for x86_64.
Create and tear down per-task virtual address spaces.
Map VMOs into a task's VAS with appropriate permissions.

#### Task Object
A kernel brick representing an authority boundary.
Contains a handle table, a virtual address space, and a collection of threads.
The kernel creates tasks through normal handle operations (kernel is task 0 with full rights).

#### Thread Object
A kernel brick representing a schedulable execution context.
Holds saved register state, stack pointer, and scheduling state.
The first thread in a task receives a handle to its own task and a handle to itself.

#### ELF Loader
Parse an ELF binary and create VMOs for each loadable segment.
Map those VMOs into the task's virtual address space at the correct addresses.
Set the thread's instruction pointer to the ELF entry point.
This is the kernel's one built-in binary format loader -- other formats can be handled by userspace shim loaders.

#### Syscall Entry
A single entry point using SYSCALL/SYSRET with register-passed arguments.
Dispatches through the three-path pipeline -- handle lookup, rights check, then operation.
OTPs are not required for this milestone; the pipeline dispatches directly to kernel-internal handlers.

#### Context Switching
Save and restore full register state between kernel and user threads.
Triggered on syscall entry/exit and on scheduler preemption.

#### Serial Write Syscall
The one syscall the hello world program actually calls.
Takes a buffer and length, writes to the UART.

#### Scheduler
Round-robin scheduling across runnable threads.
Picks the next runnable thread on timer tick or yield.
No priority system -- just simple rotation.

#### Task Termination
When the last thread in a task exits, the task enters TERMINATED.
The kernel's reaper thread eventually walks the task's handle table and closes everything.
The kernel then drops to the interactive shell.
Zombie states are acceptable -- crashing is not.
