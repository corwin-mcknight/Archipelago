# Architecture
Archipelago is an operating system that is designed to be minimal, modern, and secure. It loosely aligns itself more with UNIX but does not attempt to be compatible.

Archipelago's kernel uses a handle-based approach to manipulate kernel objects. This allows for gated entry and a baked in capabilities model.

## Kernel Objects
A kernel object is a piece of a kernel subsystem that is effectively an interface to that subsystem. They can be thought of as tiny state machines that domain specific to each subsystem that can be manipulated in user space to some degree via system calls. All kernel objects are referenced counted to prevent de-allocation, and can be accessed from multiple CPUs or threads at once (locking is handled internally per object). Userspace never gets it's hands on the data stored in a kernel object, it will instead recieve a handle. 

A handle to user space is a opaque identifier that are used to access the kernel objects. Handles are used to gate access to kernel objects, and allow for a capabilities model to be implemented. Handles also represent ownership of an object.

### Handles
A handle is a unique identifier stored in the kernel handle table. Userspace only will see the unique identifer, while the kernel knows how to route that handle to the correct kernel object to perform operations on it.

The handle table stores:
* A pointer to the kernel object
* The type of the kernel object pointed to.
* The entitlements of this handle.
* Bound process id.

A handle's kernel object creates a reference to it. When all handles are closed, the kernel object is destroyed. Handles are either manually closed, or automatically closed when the process exits.

A function that recieves its handle will be able to access the kernel object, and will be able to manipulate it. It will check the entitlements and the type of the kernel object to make sure it's allowed to access it. This means that any function that recieves a handle is able to fail with a `HND_INVALID` error code.

Entitlements on the handle can be modified via system calls. However, a handle can only be duplicated at the same permissions, and can only be modified to have less permissions. Handles can be transfered between processes.

## Memory allocation
Archipelago's kernel contains the page-frame allocator, the physical memory manager, the virtual memory manager, and allows access via handles to VMOs.

Generally, kernel objects are allocated via a SLAB arena allocator created for each specific kernel object type. A minimum amount is statically allocated at boot time until the kernel is fully initialized and can manage it's memory dynamically. The system that allows for this to happen safely is the Archipelago Unified Memory Interface  (UMI, pronounced oo-me). As kernel objects are allocated and perfectly aligned to the page, there is no fragmentation and all of memory can be perfectly used by them.

Dynamic data can be allocated on the kernel's unified heap. The kernel has a dynamic temporary memory pool that can be used for buffers.

UMI handles wiping of previous data, as it tracks on a per proto-object basis it's allocation status. Freeing is also handled by UMI, as it will mark the object as free and wipe it at some point before it's next use. This means that allocating and de-allocating a kernel object is incredibly fast, as it is just a matter of updating a bitmask.

## The Scheduler

Each CPU has it's own scheduler that is responsible for scheduling the threads on that CPU. 

Each scheduler has a several run priority queues of threads that are ready to run.

- Real time, will not be preempted.
- Numerical priority, 0-30, higher is better.
- Idle, never run unless no other threads are ready.

A thread has a timeslice associated with it. 

When a thread is scheduled, it will run until it either blocks, or the timeslice expires. If the timeslice had previously expired, it will recieve a full timeslice. If a thread only has a small amount of time, it will recieve a minium timeslice and be immediately rescheduled to the head of the queue.

On reschedule, a thread will be potentially moved to a different CPU depending on the load of the CPUs, respecting affinity. 

A threads priority will be dynamically scaled to keep the system priorized towards interactive tasks. Threads that are not real time will recieve penalties for using the whole timeslice, and be rewarded for yielding.


## Filesystem

The filesystem is general read-only and is mounted at boot time from a collection of packages stored in the store. These images are mounted as if they're files. Modifying system files requires mounting files in the store and writing to the store, which is usually restricted. The user has a read-write parition in their home. Each package can be signed and this checked by the kernel as it initializes packages.

The user is allowed to run any software that they want and build software freely within writable spaces.