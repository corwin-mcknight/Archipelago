# IPC Primitives

> [!info] Design
> This feature is not yet implemented. This page describes the planned design.

The kernel provides several IPC primitives for communication between processes and between processes and servers.
All are [[Object Model|kernel objects]] accessed through capability handles, and all operations go through the [[Object Model#Three-Path Dispatch|three-path dispatch model]].
IPC is the fallback path -- when [[Object Transaction Programs]] cannot reject or complete an operation in-kernel, the operation is packaged as a message and forwarded to the owning server.
See [[Design Principles#Performance through avoidance, not speed]].

## Messages
A message is the unit of data that moves through the IPC system.
Every message has three regions:
- **Metadata** -- kernel-owned fields that describe the message.
  Includes the opcode, total message size, handle count, the invoking handle's rights, signal mask, and queue depth.
  Always available to [[Object Transaction Programs#Data access|transaction programs]] and immutable for the duration of a syscall.
- **Header** -- the first N bytes of the message payload,
  where N is declared by the server at [[Object Model#Type Registration|type registration]].
  The kernel treats these bytes as opaque, but [[Object Transaction Programs]] can inspect them on the fast path
  to make dispatch decisions without full IPC.
- **Payload** -- the remaining bytes after the header. Arbitrary data, opaque to the kernel.

Messages can also carry handles in a fixed number of **handle slots**, enabling [[#Handle Transfer|capability transfer]] between processes.
The maximum message size and maximum handle count per message are system-defined limits.

Messages on a given channel are delivered in FIFO order.
When a kernel-completed operation (path 2) is followed by a dispatched operation (path 3) on the same object, the kernel flushes any kernel-held state before dispatching.
See [[Object Model#Ordering guarantee]].

## Channels
A channel is a bidirectional, point-to-point message-passing primitive.
Creating a channel yields a pair of handles -- one for each endpoint.
Each endpoint is a separate kernel object with its own signal state.

Messages written to one endpoint are readable from the other.
Each direction maintains FIFO ordering independently.
Channels have a bounded message queue per direction.
When the queue is full, writes return an error rather than blocking -- the kernel never introduces hidden blocking on behalf of a caller.
Clients and servers manage backpressure themselves.

### Signals
The kernel automatically manages signal bits on channel endpoints:
- `READABLE` -- set when the endpoint's incoming queue is non-empty
- `WRITABLE` -- set when the peer's incoming queue is not full
- `PEER_CLOSED` -- set when the last handle to the opposite endpoint is closed

These signals integrate with [[#Signal/Wait]] and can be inspected by [[Object Transaction Programs]].

### Server dispatch
When an operation on a server-typed object reaches path 3 of the [[Object Model#Three-Path Dispatch|three-path dispatch]],
the kernel constructs a message from the operation and writes it to the server's endpoint of a type-specific channel.
The server reads from its endpoint to receive operations.
The server processes the request and writes a response message back on the channel,
which the kernel delivers to the waiting client.

A server that manages many object types or many objects can bind its channel endpoints to a [[#Ports|port]] for multiplexed waiting.

## Ports
A port is a multi-producer, single-consumer message queue.
Unlike channels (which are point-to-point), ports aggregate events from multiple sources into a single waitable queue.

### Signal binding
Processes can bind an object's signals to a port.
When a bound object's signal state transitions to match the registered mask, the kernel queues a **packet** on the port.
Each packet contains:
- The source object's handle ID
- The signal bits that triggered the packet
- Optional server-defined payload

The port becomes `READABLE` when it has pending packets.
The process waits on the port handle rather than individual objects.

### Use case
Ports are the server-side multiplexing point.
A server that owns many objects binds their signals to a single port and runs an event loop reading packets.
This replaces the `select()`/`poll()` pattern -- rather than a syscall that takes a list of handles, the kernel delivers events through the object model.
Packets are delivered in arrival order.

## Signal/Wait
Every [[Object Model|kernel object]] carries a set of signal bits -- a bitmask where multiple signals can be active simultaneously.

### Kernel-managed signals
The lower bits are managed by the kernel automatically based on object state. Examples:
- `READABLE`, `WRITABLE`, `PEER_CLOSED` on [[#Channels|channel]] endpoints
- `READABLE` on [[#Ports|ports]] with pending packets
- Interrupt signals on [[Interrupt Model|interrupt objects]]

### Server-defined signals
The upper bits are reserved for server-defined semantics.
The kernel sets and clears them on behalf of the server -- via [[Object Transaction Programs#Signals|OFP signal instructions]] or explicit syscalls -- but never interprets what they mean.
A server could use the same signal bit to mean "data available" or "connection closed."
See [[Design Principles#The Kernel Is Taught]].

### Waiting
A process waits on a handle by specifying a set of signal bits it is interested in.
The wait blocks the calling thread until any of the requested bits are asserted on the object, or a timeout expires.
This is the mechanism that unblocks driver servers when [[Interrupt Model|interrupt objects]] fire.

For waiting on multiple objects simultaneously, processes use [[#Ports|ports]].
Binding objects to a port converts signal transitions into port packets, enabling a single wait point for arbitrarily many sources.

### OFP integration
[[Object Transaction Programs]] can set and clear signals as part of completing an operation in-kernel (path 2).
For example, a write that fills a buffer can atomically set `READABLE` on a paired output object
using [[Object Transaction Programs#Cross-object operations|cross-object signal instructions]],
completing the entire write-read notification cycle without IPC.

## Polling
Polling checks an object's current signal state without blocking.
It returns immediately with the active signal bits, regardless of whether any match the caller's interest.
Polling is the non-blocking counterpart to [[#Waiting|waiting]].

The kernel never spins on a condition internally.
All waiting is event-driven -- signal transitions wake blocked waiters directly.

## Handle Transfer
Handles are transferred between processes through [[#Channels|channel]] messages.
A message can carry up to a system-defined maximum number of handles alongside its data payload.

When a handle is written into a message, it is [[Handle Table#Transfer|transferred]] --
**removed** from the sender's [[Handle Table|handle table]] and installed in the receiver's handle table
with a new handle ID but the same rights.
The object's reference count is unaffected during transfer because the reference moves rather than being copied and released.

To transfer a handle with reduced rights, the sender first [[Handle Table#Duplicate|duplicates]] the handle with a restricted rights mask and transfers the duplicate.
The original handle remains in the sender's table.
This composes the two handle operations rather than requiring a special combined primitive.

A transfer right on the channel handle gates whether handle transfer is permitted through that channel.
A channel without transfer rights can carry data messages but not handles.
Transfer never creates rights -- the transferred handle has at most the rights the sender held,
preserving the capability model's [[Object Model#What is a handle?|monotonic rights property]].

## Relationship to Other Subsystems
- [[Object Model]] -- IPC primitives are kernel objects in the object model
- [[Object Model#Three-Path Dispatch]] -- the dispatch pipeline that terminates at IPC
- [[Object Transaction Programs]] -- programmable layer that can avoid IPC entirely
- [[Handle Table]] -- handle lookup is the entry point for dispatch; handle transfer modifies tables across processes
- [[Server Lifecycle]] -- servers receive operations via IPC; pending IPC is discarded on crash
- [[Interrupt Model]] -- interrupts as objects use signal/wait to notify driver servers
- [[Design Principles#Performance through avoidance, not speed]] -- IPC is the fallback, not the fast path
