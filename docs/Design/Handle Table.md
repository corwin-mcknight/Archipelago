# Handle Table

> [!info] Partial Implementation
> The handle table is implemented with create (via emplace), duplicate, close, type-safe get, and info.
> Transfer between tables, kernel-owned tables, and dispatch routing are not yet implemented.

The handle table is a per-process data structure that maps handle IDs to `(object, rights)` pairs.
It is the gateway to the [[Object Model]] -- every operation on a kernel object begins with a handle table lookup.

## Structure
Each entry contains:

- **Object reference** -- `ktl::ref<Object>` managing object lifetime through external reference counting
- **Rights** -- capability bitfield, only reducible (see [[Object Model#What is a handle?]])
- **Generation counter** -- 32-bit counter incremented each time a slot is recycled,
  preventing stale handle IDs from resolving to a different object (use-after-free prevention)

Handle IDs combine a 32-bit index and a 32-bit generation counter.
When a handle is closed, its slot is returned to a free list for reuse.
The generation counter on the recycled slot ensures that any outstanding references to the old handle ID will fail validation rather than silently resolving to whatever object now occupies that slot.

The handle table grows dynamically -- it starts empty and allocates entries in batches as needed.
Handle creation only fails on memory exhaustion, not on a fixed capacity limit.
Entry internals are never exposed to callers; all access goes through typed accessors that return value copies or borrowed pointers to heap-allocated objects.

## Lookup
Lookup takes a handle ID and validates it in a fixed sequence: bounds check, generation check, and liveness check (object reference non-null).
Invalid or stale handles return `RESULT_HANDLE_INVALID`.

The primary accessor is `get<T>(id, required_rights)`, which performs type validation (comparing the object's stored type ID against the expected type) and rights checking in a single locked operation.
It returns a borrowed pointer to the concrete object type, or a typed error (`RESULT_WRONG_TYPE`, `RESULT_RIGHTS_VIOLATION`).
A separate `info(id)` accessor returns a value-copy snapshot of handle metadata (rights, type ID, object ID) that is safe to hold across table mutations.

## Handle Operations
Four operations define the handle lifecycle.
They are composable primitives -- combined operations like "duplicate and transfer" are expressed as a duplicate followed by a transfer, keeping the operation set small and auditable.

### Create
The primary creation path is `emplace<T>(rights, args...)`, which constructs the object and its handle atomically.
Objects should not exist outside of a handle table -- emplace enforces this by combining allocation, construction, and handle creation into a single call.

If the free list is empty, the table grows by allocating a batch of new entries.
The object is wrapped in a `ktl::ref<Object>` with reference counting managed by the control block.

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

The `get<T>(id, required_rights)` accessor performs this check by calling the object's `type_id()` method and comparing it against the expected type's compile-time `TYPE_ID`.
Mismatches are rejected with `RESULT_WRONG_TYPE` before the operation reaches the object.
Rights are validated in the same operation -- `RESULT_RIGHTS_VIOLATION` is returned if the handle lacks the required rights.

On success, `get<T>()` returns a raw pointer to the concrete object type via `static_cast`.
The pointer is borrowed -- the handle table owns the object's lifetime through its `ktl::ref<Object>`, so the pointer remains valid as long as the handle is open.

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
