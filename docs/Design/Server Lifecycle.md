# Server Lifecycle

> [!info] Design
> This feature is not yet implemented. This page describes the planned design.

Servers are privileged userspace processes that register [[Object Model|object types]] with the kernel.
They are the sole owners of type semantics -- the kernel provides mechanism, servers provide meaning.

## Registration
Type registration happens at boot time. The flow is:

1. Server starts
2. Server queries system information (page size, architecture, available capabilities)
3. Server constructs its type registration: name, rights, [[#Storage model selection|storage model]]
4. Server calls the registration syscall
5. On success: type is assigned an ID, and the server can begin accepting operations
6. Authorized code may attach [[Object Transaction Programs|transaction programs]] to the type at runtime
7. The kernel validates each attached program (structural, bounds, authority, and budget checks)
8. On failure: registration or attachment error. For boot-critical servers, this prevents the system from starting.

### Storage model selection
Servers choose the [[Object Model#Storage Models|storage model]] based on system properties discovered in step 2.
The same server codebase may register differently on different architectures.
Because the OS is a [[Plume|single source tree]], much of this logic can be resolved at compile time.

## Crash and Recovery
### Crash protocol
When a server process dies:

1. Kernel detects server death
2. **All objects** of the server's type(s) are marked dead
3. **All handles** to those objects are invalidated across every process, including [[Handle Table#Kernel-Owned Handle Tables|kernel-owned handle tables]]
4. Any pending IPC to the server is discarded
5. Kernel-managed storage (inline buffers, pages) is cleaned up
6. Slab entries are zeroed and freed; page mappings are torn down

Clients holding handles receive `ERR_BAD_HANDLE` (or equivalent) on their next operation.
There is no partial recovery and no zombie state.
See [[Design Principles#Fail hard, recover clean]].

### Hung server detection
A server that hangs is indistinguishable from a slow server.
Without intervention, every client with a handle to that type blocks indefinitely.
The kernel uses a **watchdog/heartbeat mechanism** to detect unresponsive servers.
When the watchdog fires, the kernel declares the server dead and follows the crash protocol.

This is critical because blocked clients may include other servers (disk, init), and a cascading hang can freeze the entire system.

### Restart and re-registration
When a server restarts:

1. Server goes through the normal [[#Registration]] flow
2. Type ID may be reused or reassigned (implementation decision)
3. Authorized code can re-attach the same [[Object Transaction Programs|transaction programs]] because they are stateless
4. For [[Object Model#Page-backed|page-backed]] types: if the kernel retained the data pages, the server can inspect them and potentially recover data state
5. New handles can be created; old handles are permanently invalid

The server is responsible for its own state recovery.
The kernel provides the mechanism (retained pages, clean type re-registration) but does not reconstruct server-side state.

## Server Privileges
The ability to register an object type is gated behind a capability.
Not every process can create types -- only designated servers with the appropriate rights.
This prevents unprivileged processes from polluting the type namespace or consuming kernel resources (slab allocator pools, policy evaluation overhead).
