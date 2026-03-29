# Handle Table

> [!info] Design
> This feature is not yet implemented. This page describes the planned design.

The handle table is a per-process data structure that maps handle IDs to `(object, rights)` pairs.
It is the gateway to the [[Object Model]] -- every operation on a kernel object begins with a handle table lookup.

## Structure
Each entry contains:

- **Object reference** -- ref-counted pointer to the [[Object Model|kernel object]]
- **Rights** -- capability bitfield, only reducible (see [[Object Model#What is a handle?]])
- **Type identifier** -- identifies the object's registered type,
  cached at handle creation time so the lookup path can determine the object's type without following the object pointer
- **Generation counter** -- incremented each time a slot is recycled,
  preventing stale handle IDs from resolving to a different object (use-after-free prevention)

Handle IDs are indices into the table.
When a handle is closed, its slot is returned to a free list for reuse.
The generation counter on the recycled slot ensures that any outstanding references to the old handle ID will fail validation rather than silently resolving to whatever object now occupies that slot.

Each process's handle table has a configurable capacity limit.
Attempts to create handles beyond this limit fail, preventing resource exhaustion from runaway handle creation.

## Lookup
Lookup takes a handle ID and validates it in a fixed sequence: bounds check, then generation check.
Invalid or stale handles return `ERR_BAD_HANDLE`.
Valid lookups yield the entry's object reference, rights, and type identifier -- everything the dispatch path needs without touching the object itself.

## Handle Operations
Four operations define the handle lifecycle.
They are composable primitives -- combined operations like "duplicate and transfer" are expressed as a duplicate followed by a transfer, keeping the operation set small and auditable.

### Create
Allocates a free slot, associates it with an object, and assigns an initial set of rights.
The rights are capped by the type's valid rights mask -- a handle cannot be created with rights that are meaningless for its object type.
The type identifier is captured from the object's type descriptor and cached in the entry.
The object's reference count is incremented.

Create is the only way handles come into existence.
Every other operation that produces a handle (duplication, transfer) is defined in terms of creating a new entry.

### Duplicate
Creates a new handle in the same table pointing to the same object, with equal or fewer rights.
The caller provides a rights mask that is ANDed with the existing handle's rights -- this enforces the [[Object Model#What is a handle?|monotonic rights property]].
The object's reference count is incremented (two handles now reference it).

Duplication is the mechanism for rights attenuation.
Before passing a handle to less-trusted code or transferring it to another process, the caller duplicates it with a restricted mask and operates on the duplicate.

### Transfer
Moves a handle from one table to another atomically.
The source entry is removed and a new entry is created in the destination table.
The handle ID changes (it is an index into a different table), but the object reference and rights are preserved.
The object's reference count is unaffected -- the reference moves rather than being copied and released.

If the destination table is full, the transfer fails and the source handle remains intact.
At no point does the handle exist in both tables or neither table.

Transfer is the mechanism behind [[IPC Primitives#Handle Transfer|capability passing through channels]].
A transfer right on the channel gates whether transfer is permitted.

### Close
Destroys a handle.
The object's reference count is decremented.
If this was the last reference, the object is destroyed (see [[Object Model#Object lifetime]]).
The table slot is returned to the free list with an incremented generation counter, ensuring future lookups against the old handle ID fail cleanly.

Closing an already-closed handle returns `ERR_BAD_HANDLE` through the generation check -- there is no special "double close" error and no undefined behavior.

## Type-Safe Access
When the kernel processes an operation, it often needs to verify that a handle refers to a specific kind of object -- a channel operation should not succeed against a VMO handle.

The type identifier cached in each handle entry makes this check possible without following the object pointer.
The kernel compares the entry's type identifier against the expected type.
Mismatches are rejected with an error before the operation reaches the object or enters the [[Object Model#Three-Path Dispatch|dispatch pipeline]].

The type identifier is already present in the entry alongside the rights and generation counter, so type validation adds no additional memory access to the lookup path.

For the [[Object Model#Three-Path Dispatch|three-path dispatch]], the same cached type identifier routes the operation to the correct type's [[Object Transaction Programs|transaction programs]].
Type validation and type routing are the same check -- not two separate steps.

## Kernel-Owned Handle Tables
The kernel itself sometimes needs to hold references to objects -- interrupt objects, boot-time channel endpoints, or internal bookkeeping resources.
Kernel-owned handle tables serve this purpose.

They are structurally identical to per-process tables: same entry format, same generation counters, same operations.
The difference is ownership and visibility.

### Isolation
Kernel tables are not reachable from userspace.
No handle ID that a process can present through a syscall will resolve against a kernel table.
A process cannot name, inspect, or revoke a kernel-held handle.
This is a hard boundary -- kernel handles exist outside the userspace capability model entirely.

### Sharing objects with processes
When the kernel wants to give a process access to an object it holds internally, it creates a new handle in the process's table with appropriate rights.
The kernel's own handle is unaffected.

The kernel never transfers its own handles -- it always creates fresh ones in the target table.
This ensures the kernel retains its reference regardless of what the process does with its handle.

### Server crash
When a [[Server Lifecycle#Crash and Recovery|server crashes]], kernel-held handles to that server's objects are invalidated through the same mechanism as process-held handles.
The kernel is not exempt from the [[Design Principles#Fail hard, recover clean|crash protocol]].

## Dispatch
The handle table is the entry point for the [[Object Model#Three-Path Dispatch|three-path dispatch]]:

1. Look up handle by ID
2. Validate generation
3. Check rights
4. Route by type identifier to the object's [[Object Transaction Programs|type dispatch]]

This path is identical regardless of object type.
The first three steps use only data from the handle entry itself.

## Relationship to Other Subsystems
- [[Object Model]] -- objects are what handles reference; type descriptors define valid rights and type identifiers
- [[Object Model#Three-Path Dispatch]] -- the dispatch pipeline that begins at handle lookup
- [[IPC Primitives#Handle Transfer]] -- handle transfer through channel messages is built on the [[#Transfer]] operation
- [[Object Transaction Programs]] -- dispatch enters transaction programs after handle lookup,
  generation check, rights check, and type routing
- [[Server Lifecycle#Crash and Recovery]] -- server crash invalidates handles across all tables,
  including [[#Kernel-Owned Handle Tables|kernel-owned ones]]
- [[Memory Subsystem]] -- handle table memory is managed by the kernel's allocators
