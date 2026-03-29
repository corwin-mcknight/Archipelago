# Object Transaction Programs

> [!info] Design
> This feature is not yet implemented. This page describes the planned design.

Object Transaction Programs (OTPs) are a programmable dispatch layer that sits between the handle table and IPC dispatch.
Servers attach small programs -- composed of safe opcodes -- to their object types, and the kernel executes these programs to short-circuit the IPC path where it can safely do so.

## Philosophy
### Graduated opacity
OTPs embody [[Design Principles#Graduated opacity]].
The kernel doesn't understand server semantics, but it can enforce structural invariants and perform mechanical operations on behalf of the server.
See also [[Design Principles#Performance through avoidance, not speed]].

The analogy is a JIT compiler: the server is the interpreter (handles everything correctly), the programs are JIT-compiled fast paths (observed invariants and declared operations).
If the fast path can't handle a case, you fall back to the interpreter.
The kernel never guesses -- it only acts on what the server explicitly declared.

### Safety by construction, not verification
The Berkeley Packet Filter (BPF) and its extended variant (eBPF) rely on a verifier to prove programs are safe -- an increasingly complex problem as the instruction set grows.
OTPs take the opposite approach: **the instruction set itself makes unsafe behavior unrepresentable**.

- No arbitrary memory access -- only typed loads/stores against object-owned regions
- No backward jumps -- forward-only control flow guarantees termination
- No division -- eliminates div-by-zero complexity
- No side effects outside object storage and signals

Any composition of safe opcodes is safe.
No halting problem.
No verifier.
Maximum execution time is bounded by program length, calculable at load time.

The closest historical analog is the N64's RSP microcode: a restricted instruction set operating on fixed local memory.
You couldn't escape the sandbox regardless of what microcode you wrote.
Expressiveness came from composing simple, safe operations.

### The three-path contract
Every program **must** terminate with one of three outcomes, preserving the [[Object Model#Three-Path Dispatch|three-path model]]:

- **Reject** -- operation is invalid, return error
- **Complete** -- operation handled entirely in-kernel
- **Dispatch** -- forward to server via IPC

Programs are an optimization layer, not a departure from the dispatch model.
A server that uses no programs still receives everything via IPC.

### The completion boundary test
Adding new completable instructions expands the kernel's surface area.
The test for whether an instruction belongs in the kernel: **could it be implemented with a single lock-free or single-lock operation on data the kernel already holds?**
Buffer write -- yes.
Signal set -- yes.
"Parse this as a DNS query and cache the response" -- absolutely not.

## Instruction Set
### Registers
A fixed register file of general-purpose 64-bit registers.

### Data access
Data access instructions operate on bounded, kernel-managed regions only.
No raw pointer arithmetic, no access outside declared storage.
Width is restricted to standard power-of-two sizes.
Offsets are bounds-checked against the object's declared storage size at **program creation time** (not execution time -- this is a static guarantee).

This only applies to fixed, declared storage.
OTPs do **not** support resizing or dynamically-shaped storage.
That is intentional.
This is a progressive addition to otherwise opaque objects, not a replacement for full server handling.

Programs can inspect three categories of data:
- **Metadata** -- rights, opcode, [[IPC Primitives#Messages|message]] size, handle count, queue depth, signal mask.
  Always available, always safe.
  Notably, the [[Object Model#What is an object?|object ID]] is not accessible through metadata -- transaction programs cannot leak object identity.
- **Header** -- the first N bytes of the message,
  where N is declared by the server at type registration.
  A server-defined structure that the kernel treats as opaque bytes.
- **Buffer** -- inline or page-backed storage associated with the object. Supports both reads and writes.

### Arithmetic
Basic arithmetic and bitwise operations. Shift amounts are immediate values.

### Comparison
Instructions for comparing register values and testing bitmasks. Results set a condition flag used by control flow.

### Control flow
Conditional forward jumps based on the condition flag.
**Forward-only jumps.**
This is the critical constraint.
No backward jumps means no loops.
Programs always progress toward termination.
The maximum instruction count executed equals the program length.
Worst case is a straight-line run through every instruction.

### Signals
Instructions to set and clear signal bits on the current object.

### Cross-object operations
Instructions to load from, store to, and signal paired objects.
Object references are declared when the program is created (see [[#Program Creation and Authority]]).
At creation time, the kernel validates that the creating handle has rights to both objects.
Execution then operates on pre-validated object references without continuously re-checking handle authority on the hot path.

Referenced objects remain alive because all objects are [[Object Model#Object lifetime|reference counted]].
A program that names secondary objects holds references to them for as long as the program exists.

Cross-object composition is what separates OTPs from simpler filter systems.
A pipe server that wants "write to input A completes as a read from output B without IPC" can express that relationship directly.

### Termination
Every valid program must end with one of the three outcomes from the [[#The three-path contract|three-path contract]].
Programs without a reachable termination instruction are rejected at creation time.

## Program Creation and Authority
### The capability model
Program creation authority derives from the [[Object Model#What is a handle?|handle system]].
The handle that created the object type is the root of trust.
Programs can only be created by handles with appropriate authority.

A dedicated right controls who can create transaction programs.
This right is separate from other type-creator rights, enabling fine-grained delegation:

- A server creates a type with full authority
- It passes a handle with program-creation rights (but not write or transfer) to a trusted client
- That client can create custom transaction programs for the type without full type-creator access

This is a large trust boundary.
Attaching programs to a type means granting another party the ability to define in-kernel fast paths for that type.

### Program attachment model
Programs are attached to **types after type registration, at runtime**.
They are not part of the initial type registration payload.

Only handles with the appropriate authority for the type may attach programs.
In practice, this usually means the server itself or another program that the server has deliberately trusted with program-creation rights for that type.

Once attached, a program becomes part of the type's dispatch behavior for all objects of that type.

### Cross-object program authority
Programs that use cross-object instructions must declare their object references at creation time.
The kernel validates:

1. The creating handle has rights to the primary object's type
2. The creating handle has rights to each referenced secondary object
3. All buffer offsets in cross-object instructions are within bounds of the referenced objects' declared storage

These validations happen once at creation.
Execution does not continuously re-check handle authority.

### Static validation at creation
When a program is submitted:

1. **Structural validation** -- every code path reaches a termination instruction, all jumps are forward and in-bounds
2. **Bounds validation** -- all buffer offsets are within declared storage sizes
3. **Authority validation** -- the creating handle has rights to all referenced objects
4. **Budget validation** -- program length is within the system limit;
   worst-case cycle cost is calculated and checked against a configurable maximum

If any validation fails, program creation fails. There is no partial acceptance.

## Program Lifecycle
### Declaration
Programs are **attached to types after registration** by an authorized handle.
They cannot be modified in place once attached.
A new program must be attached as a new version or replacement according to the type's policy.
[[Object Model#Type Registration|Types are never unregistered]].

### Statelessness
Programs are stateless -- they are pure instruction sequences with no memory of previous executions.
This means they can be trivially re-applied on [[Server Lifecycle|server restart]] without any state recovery.

### Versioning
Programs are versioned. This handles two concerns:

1. **New instructions** -- a kernel update adds new opcodes.
   Servers built against the old kernel can flag which instructions they depend on,
   so an unknown opcode fails attachment rather than being silently skipped.
2. **Instruction semantics** -- if the kernel changes the behavior of an existing instruction,
   the version ensures servers know which behavior they're getting.

Because the OS is built as a [[Plume|single source tree]], versioning is largely a compile-time concern.
Mismatched versions are build errors, not boot errors.

### Unknown instructions and validation failure
When a program is processed for attachment:

- **Unknown opcode** -- hard failure. Attachment fails.
- **Validation failure** -- hard failure. Attachment fails.

This is intentional -- an invalid or unknown program is a bug, not a runtime condition.

## Data Inspection Safety
### Metadata inspection
Always safe.
Metadata fields (rights, opcode, size) are kernel-owned and immutable for the duration of a syscall.

### Buffer inspection on shared pages
When programs inspect data in [[Object Model#Page-backed|shared pages]], the server could mutate the data mid-execution.
Two approaches:

- **Brief page lock** -- lock the page for the duration of program execution.
  Simple, but lock duration scales with program length.
- **Snapshot-on-execute** -- copy the inspected bytes into a small kernel-side scratch buffer before execution starts.
  The lock is just the copy duration (a few cache lines), and execution runs against stable data.
  **This is the preferred approach.**

### Buffer inspection and versioning
Instructions that reference buffer offsets are coupled to the server's wire format.
If the server changes its message structure, the kernel's programs inspect the wrong bytes.
Since the OS is a [[Plume|single source tree]], this is a build-time synchronization --
programs and message structures change in the same commit.

## Examples
* **Message validation** -- check size constraints and command identifiers,
  rejecting malformed requests before they reach the server
* **Zero-copy pipe** -- copy incoming data directly to a paired output object's buffer and signal it as readable,
  completing the entire write-read cycle in-kernel with no context switch
* **Conditional dispatch** -- handle simple cases in-kernel while forwarding complex ones to the server,
  such as buffering small messages directly while dispatching large ones
* **Synchronization primitives** -- shared memory mutexes, futex-like mutexes, counting semaphores,
  read-write locks, barriers, spinlocks
* **Input dispatch**
* **Audio mixing buffer**
* **Message queues**

## Comparisons
| System | Typed objects | Capability security | Safe by construction | Cross-object composition |
|---|---|---|---|---|
| eBPF | No | No | No (needs verifier) | No |
| N64 RSP microcode | No | No | Yes | No |
| GPU command buffers | No | No | Yes | Limited |
| Zircon | Yes | Yes | N/A (no programs) | No |
| **OTPs** | **Yes** | **Yes** | **Yes** | **Yes** |

Zircon is the microkernel at the core of Google's Fuchsia operating system.

## Future Directions
- Bounded loop construct (fixed iteration count known at creation, still terminates)
- Program chaining (one program invokes another, with a call depth limit)
