# Architecture
Archipelago is an operating system that is designed to be minimal, modern, and secure. It loosely aligns itself more with UNIX but does not attempt to be compatible.

Archipelago's kernel uses a handle-based approach to manipulate kernel objects. This allows for gated entry and a baked in capabilities model.

## Kernel Objects
Kernel objects are interally managed pieces of data that effectively are tiny state machines for each subsystem of the kernel.

Kernel objects are accessed via handles, which are opaque identifiers that are used to access the kernel object. Handles are used to gate access to kernel objects, and allow for a capabilities model to be implemented. Handles also represent ownership.

### Handles
A handle is a unique identifier stored in the kernel handle table.
The handle table stores:
* A pointer to the kernel object
* The type of the kernel object pointed to.
* A checksum to detect tampering.
* The entitlements of this handle.
* Bound process id.

A handle's kernel object creates a reference to it. When all handles are closed, the kernel object is destroyed. Handles are either manually closed, or automatically closed when the process exits.

A function that recieves its handle will be able to access the kernel object, and will be able to manipulate it. It will check the entitlements, the checkum, and the type of the kernel object to make sure it's allowed to access it. This means that any function that recieves a handle is able to fail with a `HND_INVALID` error code.

## Memory allocation
Archipelago's kernel contains the page-frame allocator, the physical memory manager, the virtual memory manager, and allows access via handles to VMOs.

Generally, kernel objects are allocated in a pool of memory.
The kernel unified memory interface (kumi) decides where to allocate memory from during the boot process of the kernel.



## The Scheduler

Each CPU has it's own scheduler that is responsible for scheduling the threads on that CPU. 

Each scheduler has a several run priority queues of threads that are ready to run.

- Real time, will not be preempted.
- High - Low
- Idle, never run unless no other threads are ready.

A process has a timeslice associated with it. 
When a thread is scheduled, it will run until it either blocks, or the timeslice expires. If the timeslice had previously expired, it will recieve a full timeslice. If a thread only has a small amount of time, it will recieve a minium timeslice and be immediately rescheduled to the head of the queue.


## Filesystem

The filesystem is general ephemeral. The root partition is entirely read-only, and is mounted at boot time from a collection of packages stored in the store. Modifying system files requires mounting files in the store and writing to the store, which is usually restricted. The user has a read-write parition in their home.