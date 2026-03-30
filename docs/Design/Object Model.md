# Object Model

> [!info] Partial Implementation
> The core object primitives are implemented: Object base class, type registry, handle table, and two concrete types (Event, Counter).
> Storage models, three-path dispatch, OTPs, IPC, and server lifecycle are not yet implemented.

The kernel's object model is the foundation of the system.
Every resource -- channels, ports, sockets, interrupts, MMIO regions, DMA buffers -- is represented as a typed kernel object, accessed exclusively through capability handles.

## Core Concepts
### What is an object?
An object is anything that can be pointed to by a handle.
The base representation is deliberately minimal:

- **Type ID** -- a stored integer identifying the object's registered type.
  The full type descriptor (name, rights, storage model) lives in the type registry and is looked up by ID when needed.
- **Signal state** -- waitable signal bits for the [[IPC Primitives|signal/wait and polling]] systems
- **Object ID** -- a unique kernel-wide identifier assigned at creation from a monotonic counter.
  The object ID is not a handle and not a capability -- it exists for kernel-internal purposes:
  unambiguous identification in logs and diagnostics, and same-object identity checks across [[Handle Table|handle tables]].
  Object IDs are never reused.
- **Debug name** -- an optional nullable label for logging and diagnostics

Reference counting is external -- `ktl::ref<T>` manages object lifetime through a non-intrusive control block.
The object itself carries no refcount field.

The kernel does not interpret what an object *means*.
It knows things *about* the object (structural constraints, storage model, valid operations) but all semantic meaning is owned by the server that registered the type.
See [[Design Principles#Graduated opacity]].

### What is a handle?
A handle is a `(object, rights)` pair owned by a process.
Handles are looked up by an opaque handle ID, which is an index and generation counter into a per-process [[Handle Table]].
See [[Handle Table#Handle Operations]] for the full lifecycle (create, duplicate, transfer, close) and [[Handle Table#Type-Safe Access]] for validated typed access through handles.

Rights are a bitfield.
There are rights for reading, writing, duplicating, transferring, and signaling at minimum.
Rights can only be reduced, never expanded.
[[Handle Table#Duplicate|Duplicating]] a handle ANDs the existing rights with a mask.
There is no operation that adds rights to an existing handle.

Rights represent *potential* operations, not currently exercised ones.
It is the application's responsibility to restrict a handle's rights before passing it to less-trusted code.
A handle with write rights to a read-only type will have writes rejected by the [[Object Transaction Programs|transaction program]], but the rights themselves are not invalid -- the program simply prevents the operation.

### Object lifetime
An object lives as long as handles reference it.
When the last handle is [[Handle Table#Close|closed]], the object is destroyed.
The kernel manages this through `ktl::ref<T>`, which uses atomic reference counting in a type-erased control block.
The type registry tracks live instance counts per type for diagnostics and auditing.

When a [[Server Lifecycle|server crashes]], **all handles and objects of that type are destroyed**.
No partial recovery, no zombie objects.
The server is expected to re-register its type on restart.
See [[Design Principles#Fail hard, recover clean]].

## Type Registration
Object types are registered by privileged servers at boot time.
The ability to create an object type is gated behind a capability.
Types are **never unregistered** -- they exist for the lifetime of the system.

Registration consists of:

- **Type name** -- human-readable identifier ("event", "counter", "channel")
- **Type ID** -- for kernel-defined types, a constexpr value declared in the type's class.
  User-defined types will receive dynamically assigned IDs.
  Used by the [[Handle Table#Type-Safe Access|type-safe access]] mechanism to validate handle types without runtime type introspection.
- **Default rights** -- rights assigned when a handle is created without an explicit rights argument
- **Valid rights** -- mask of rights that are meaningful for this type
- **Storage model** -- how the kernel manages data for objects of this type (see [[#Storage Models]], not yet implemented)

[[Object Transaction Programs]] are attached later, after type registration, by authorized handles at runtime.
They are not part of the initial registration payload.

### Architecture adaptation
Because the OS is built as a [[Plume|single source tree]], servers can adapt their type registration to the target architecture at compile time.
A server might choose different storage models or policy parameters based on page size, available hardware features, or platform constraints.
See [[#Storage Models]] for an example.

The kernel also exposes system information that servers can query at runtime before registration.
There is a window between server startup and type creation where the server can discover everything it needs about the system.

### Re-registration on server restart
When a server restarts after a crash and re-registers its type, authorized code can re-attach the same transaction programs because programs are stateless.
For objects with storage backed by the kernel, the data pages may survive the server crash, potentially allowing the server to recover data state -- though all client handles are gone regardless.

## Storage Models
Each object type declares how the kernel manages its backing data.
This is decided **per-type** at registration, so every object of a given type uses the same allocator.

### None
The kernel holds no data for this object.
Every operation that passes the [[Object Transaction Programs|rejection stage]] is dispatched to the server via IPC.
Pure proxy.

### Inline buffer (slab-backed)
The kernel manages a small buffer for each object, allocated from a [[Memory Subsystem|slab allocator]].
The slab class is determined by the declared buffer size.
A system-wide limit constrains the maximum inline buffer size to prevent abuse.

[[Object Transaction Programs]] can complete reads and writes directly against this buffer without IPC,
making it ideal for high-frequency small messages.

#### Slab allocator behavior
Slab-backed objects that only the kernel touches have an efficient destruction path:
zero the data pages, mark items as freed, but defer page table teardown.
Metadata pages can be compacted and destroyed lazily.
This trades memory reclamation latency for destruction throughput -- the right trade for high-churn objects.

### Page-backed
The kernel owns data pages for the object.
The server maps these pages into its address space and operates on them directly.
The kernel can complete operations against these pages on the fast path (copy in, set dirty signal)
while the server handles complex state transitions.

#### Architecture-adaptive storage selection
The same logical object type may use different storage models on different architectures. Example:

- A type that needs 4K of storage per object
- On a **4K-page architecture**: uses the page allocator directly -- one page per object
- On a **16K-page architecture**: uses the slab allocator with 4K slabs -- four objects per page, 4x more compact

The server makes this decision based on system information available at registration time.
The kernel doesn't choose -- it just handles the request.
See [[Design Principles#Servers adapt to the system, not the other way around]].

### Mixed storage
A single object type can combine inline and page-backed storage --
a small header in a slab-allocated buffer plus a variable-size data region in pages.
The inline header is inspectable by [[Object Transaction Programs]] on the fast path without touching the data pages.

Destruction must free both the slab entry and the pages atomically.

## Three-Path Dispatch
Every operation on a handle follows the same pipeline regardless of object type.
See [[Object Transaction Programs]] for the program execution details.

```
operation arrives
    → Transaction program executes (if attached)
        → REJECT: return error
        → COMPLETE: handled in-kernel
        → DISPATCH: forward to server via IPC
    → No program: dispatch to server via IPC
```

1. **Rejected early** -- the [[Object Transaction Programs|transaction program]] determines the operation is invalid and returns an error.
   No IPC, no server involvement.
2. **Completed in kernel** -- the program executes a kernel-completable action (buffer write, signal set, etc.).
   The operation is handled entirely in ring 0.
3. **Dispatched to server** -- the program dispatches (or no program is attached),
   so the operation is packaged as a [[IPC Primitives#Messages|message]]
   and sent to the owning server via its [[IPC Primitives#Channels|type channel]].

A server that defines no programs simply receives everything via path 3.
Programs are purely an optimization -- the server must always be capable of handling any operation that reaches it.

### Ordering guarantee
When an operation is completed in-kernel (path 2) and a subsequent operation on the same object
falls through to IPC (path 3), the kernel **flushes** any kernel-held state
(e.g., inline buffer contents) to the server before dispatching the IPC message.
This preserves the client's sequential ordering expectation.

## Relationship to Other Subsystems
- [[Object Transaction Programs]] -- the programmable filter layer that drives the three-path dispatch
- [[Handle Table]] -- per-process handle storage with generation counters
- [[IPC Primitives]] -- channels, ports, messages, signal/wait, polling, handle transfer
- [[Interrupt Model]] -- interrupts as objects with handles
- [[Server Lifecycle]] -- registration, crash, recovery
- [[Device Drivers]] -- MMIO and DMA through the object model
- [[Memory Subsystem]] -- slab and page allocators backing object storage
