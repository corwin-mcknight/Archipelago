# Service Coordination

> [!info] Partial Implementation
> The coordinator (`sys/init`), the spawn syscall, the message envelope, IMAGE endowment, and the register/connect/CONNECTION protocol are implemented, with open logged policy. Manifest-based policy, connection timeouts, and the userspace loader trajectory remain planned.

Service coordination is how a program reaches a service it was not endowed with at creation.
The kernel provides no naming: names, registration, and connection policy live in one userspace task, the coordinator.
This is the name-to-channel layer only.
It is deliberately smaller than the full [[Object Model]] endgame -- no type registration, no [[Object Transaction Programs|transaction programs]], no three-path dispatch.
Those arrive later and subsume none of this: the coordinator routes *connections*; the type system will route *operations*.

## The Coordinator
The coordinator is the first and only task the kernel launches -- it is the program named init.
It is both process manager and service broker, and the two roles are one mechanism: because the coordinator spawns every other task, it already holds the parent end of every task's bootstrap channel, so **the parent mailbox is the coordinator channel**.
There is no separate broker endowment and no discovery syscall.
A task that can talk to anything at all can talk to the coordinator, because the bootstrap channel is the one channel every task is born holding.

The coordinator never exits.
Until task restart and supervision exist, it is a single point of failure; this is accepted, as it is for the kernel itself.
A task observes coordinator death as `PEER_CLOSED` on its mailbox -- the existing orphan signal.

## Spawning
The coordinator creates tasks through a spawn syscall whose source argument is a [[Memory Subsystem#Virtual Memory Manager|VMO]] handle.
Spawn returns two handles: the new task's handle, and the parent end of its bootstrap channel.
Holding an executable image's VMO *is* the authority to spawn it -- there is no ambient spawn privilege and no kernel-side list of programs.

Spawning is also where [[Standard Streams|stdio]] is wired: the coordinator mints the socket pairs behind a new task's input, output, and error streams, mails the task its ends, and routes the read ends of output and error to the console server.

Boot modules never reach this interface.
At boot the kernel wraps each module's bytes in a read-only wired VMO and mails the coordinator one IMAGE message per module on its bootstrap channel: the VMO handle rides the message, and the payload carries the exact byte size and the module's name.
Per-module mail rather than one manifest message, because a message carries only a handful of handle slots and the set of programs will outgrow them.
Modules are a stopgap for the missing initrd; when a filesystem or package server exists, it will mint VMOs the same way, and spawn does not change.

### The loader trajectory
The spawn syscall parses ELF in the kernel.
This is scaffolding, and it is the piece of this design that gets deleted.
The end state is a set of builder primitives -- create an empty task, map VMO ranges into its address space through [[Task Model#Structure|region delegation]], start a thread at an entry point -- with binary format loaders (ELF, Mach-O, others) living in the coordinator.
The kernel's ELF loader then shrinks to the boot path only, where it must still load the coordinator itself.
Builder primitives wait on VMO map operations and region handle exposure, which are their own milestone.

## Message Envelope
Every message on a coordinator channel -- and by convention, on any service channel -- begins with one fixed envelope, defined in the ABI headers alongside the syscall interface: an opcode, a status, and a transaction id, followed by a per-opcode packed struct.
Structs are native-endian and packed; same-machine IPC on a single architecture needs no serialization.
Evolution is append-only: opcodes are never reused or renumbered, and a protocol grows by adding opcodes rather than versioning.
Opcode spaces are per-protocol -- the coordinator protocol owns its own numbers, and a service's protocol owns its own.

The transaction id is what makes one channel carry many conversations.
A reply carries the request's txid, so replies can arrive arbitrarily late and interleave with unsolicited messages without ambiguity.
The [[IPC Primitives#Messages|message header region]] of the full design maps onto this envelope when type registration arrives; until then the envelope is a convention between peers.
The kernel never inspects envelopes in transit -- it speaks the convention only where it is itself a peer, as the coordinator's parent mailing IMAGE messages.

## Names and Connections
The coordinator protocol has two requests: register and connect.

**Register** claims a name -- a flat lowercase string, no hierarchy.
No handle accompanies it; a name is a claim, not an endpoint.
A registration lives exactly as long as its claimant: when a task's mailbox reports `PEER_CLOSED`, the coordinator removes its names.
Exit, crash, and kill are indistinguishable here, which is the point.

**Connect** asks for a name.
The coordinator mints a fresh channel pair, replies to the client with one end, and sends the registrant a new-connection message carrying the other end.
Every client gets a private channel: per-client hangup, per-client flow control, and -- later -- per-client rights, since the coordinator can restrict either end before handing it over.
A server's event loop learns of new clients as messages on the mailbox it already watches, and binds each arriving channel to its [[IPC Primitives#Ports|port]].

A connect for a name nobody has registered parks until the name appears; the txid makes the late reply unambiguous.
Boot ordering therefore does not matter -- a client may connect before its server has started.
A name that never appears parks the request forever, which is acceptable while clients wait with bounded timeouts; a negative-reply policy can be added as an opcode later.

## Policy
Registration and connection are open: any task may claim any free name, any task may connect to anything, and the coordinator logs every request.
What this design builds is not the rules but the enforcement point.
Every request arrives on a mailbox whose provenance the coordinator knows -- it spawned the sender from a named image -- so identity is unforgeable without any kernel involvement.
When program manifests exist, per-program rules (which names a program may register, which services it may reach) drop into the coordinator without new mechanism.
See [[Design Principles#Bricks and houses]]: naming and policy are a house, and the kernel supplies only the bricks it already had.

## Relationship to the Type System
[[Server Lifecycle]] describes servers that register *object types* with the kernel; that machinery is unimplemented and unchanged by this design.
Name registration with the coordinator is a different, earlier layer: it routes a client to a channel, and everything after that is convention between the two peers.
When type registration lands, a server will likely do both -- claim a name for discovery, register types for kernel-mediated dispatch -- and the coordinator's manifest is the natural place to record who may do which.

## Relationship to Other Subsystems
- [[Task Model]] -- the coordinator is every task's parent; spawn, kill, and exit status are task operations
- [[IPC Primitives]] -- channels carry the protocol; ports multiplex server event loops; handle transfer moves the minted ends
- [[Syscall Interface]] -- spawn is a handle syscall on a VMO; there are no discovery syscalls
- [[Server Lifecycle]] -- the future type-registration layer this design deliberately does not touch
- [[Design Principles]] -- bricks and houses; the kernel is taught, not grown
