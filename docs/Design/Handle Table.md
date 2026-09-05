# Handle Table
The handle table maps a task's handle IDs to object references and rights.
Userspace can name only entries in its calling task's table; the kernel task has a separate table for internal handles and channel-transfer escrow.
The table provides ownership and capability checks for the [[Object Model]].

## Identity and Lifetime
A handle contains a 32-bit slot index and a generation counter.
Closing or taking a handle invalidates that ID before the slot can be reused.
Generations are capped at `0x7fffffff`, preserving the sign bit used to distinguish handles from syscall errors; saturated slots are retired instead of wrapping.
A fresh table allocates slot zero first for the bootstrap channel.

Each live entry holds one owning `ktl::ref<Object>`, its rights, and its generation.
The table reserves a complete batch before adding slots and tracks reusable slots in a free list.
Allocation failure leaves the existing entries and free list unchanged.
An object can also be owned by other handles, mappings, or kernel references; closing a handle destroys the object only when its last reference is released.

## Lookup
`get<T>(id, required_rights)` checks handle validity, object type, and rights under one lock, in that order.
It returns an owning `ktl::ref<T>` or `handle_invalid`, `wrong_type`, or `rights_violation`.
The reference keeps the object alive even if the handle is subsequently closed.

`verify()` performs the same checks and returns an object reference plus the handle's rights.
It accepts any object type when no expected type is supplied.
Use it for generic object operations or when the caller needs the rights; use `get<T>()` for typed operations.
`snapshot()` copies live handle metadata for inspection without exposing table entries.

## Creation and Duplication
`emplace<T>()` constructs an object and inserts a handle; `insert()` gives an existing object a handle.
Requested rights must fit the object's registered type contract.
Both paths can fail without publishing a handle.

`duplicate()` requires `RIGHT_DUPLICATE` and creates another handle to the same object.
The new rights are the source rights ANDed with the supplied mask, so duplication cannot add authority.

## Rights Restriction
`SYS_HANDLE_RESTRICT(handle, rights, mode)` changes rights in place without changing the handle ID or object reference.
Both modes operate under the table lock, require no special right, and allocate nothing:

- `RETAIN` replaces rights with an exact subset. Bits absent from the current rights, including nonzero bits above the 32-bit field, fail with `rights_violation` and leave the handle unchanged.
- `REMOVE` clears selected bits. Absent or unknown bits are ignored; only an invalid handle can fail this mode.

Equal rights and zero rights are valid. Unknown modes return `invalid_operation` for a valid handle.
Other handles to the object are unaffected, and operations already verified may finish with their acquired authority.
A zero-rights handle still owns the object and can be inspected, restricted, or closed.

## Close and Transfer
`close()` invalidates a handle and releases its reference after unlocking, because object destructors may re-enter the table.
`clear()` releases all live handles and rebuilds the free list, also dropping references outside the lock.
`take()` invalidates a handle but returns its owning reference and rights to the caller.

Channel transfer uses `take()` and `insert()` to move ownership through the kernel task's escrow table and into the receiver's table.
The transfer right on the sending channel handle gates this operation.
Transfers are not atomic across tables: once a send starts consuming handles, later failures close those already consumed.
A dropped message closes its escrowed handles; a receiver insertion failure closes that handle and reduces the reported delivery count.
The syscall contract is defined in `src/sys/kernel/includes/abi/syscall.h`; see [[IPC Primitives#Handle Transfer|Handle Transfer]] for the broader design.

## Dispatch
One syscall switch calls ordinary handlers.
Typed object operations use `get<T>()`; generic metadata and signal operations use `verify()`.
Close, duplicate, and rights restriction call table operations directly, without a preliminary lookup.
The handle table owns validation and reference acquisition; object operations run after its lock is released.

Uniform server routing and [[Object Transaction Programs]] belong to the planned [[Object Model#Three-Path Dispatch|three-path dispatch]] design.
Server-crash revocation is also future work; see [[Server Lifecycle#Crash and Recovery|Crash and Recovery]].
