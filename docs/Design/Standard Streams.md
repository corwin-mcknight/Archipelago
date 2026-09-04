# Standard Streams

> [!info] Design
> This page describes the planned stdio wiring. Sockets and in-place handle rights restriction are implemented.

Standard streams are how a program receives and produces ordinary bytes -- the role stdin, stdout, and stderr play in UNIX, rebuilt on capabilities.
A stream is not a file and there are no descriptor numbers: a program is born holding [[IPC Primitives#Sockets|socket]] ends, and everything downstream of that is somebody else's decision.

## Streams Are Sockets
A program's output is a socket end it can write; its input is a socket end it can read.
The program does not know what is behind either -- a console server, another program's input, or nothing at all.
That polymorphism is the capability model doing what UNIX needed the file abstraction for: the indirection is the handle itself, so any object-holding program composes with any byte consumer.
A pipeline between two programs is one socket pair, the write end endowed to the producer as output and the read end to the consumer as input, with no mechanism beyond spawn-time wiring.

`SYS_HANDLE_RESTRICT` narrows rights in place to make the ends unidirectional -- output is a socket end without the read right, input one without the write right -- so the stdio shape is policy applied by the spawner, not a property of the primitive.

## The Stdio Endowment
Every task the [[Service Coordination|coordinator]] spawns is endowed with three streams: input, output, and error.
Error is distinct from output for the classic reason -- diagnostics must survive the day output is piped into something that is not a human.
The coordinator may wire error and output to the same sink, but the distinction exists from the first program.

The endowment travels as one stdio message on the [[Task Model#Bootstrap|bootstrap channel]], carrying the three socket ends, sent by the spawner immediately after spawn.
The kernel writes the bootstrap message before spawn returns, so the stdio message is deterministically the second message a spawned task reads.
This is convention, not mechanism: the kernel neither knows nor enforces stdio, and a spawner that wires no streams simply does not send the message.
The C runtime drains the bootstrap and stdio messages before main and exposes the three ends to the program.

Input is defined but unwired today: the slot exists in the message, and the coordinator leaves it empty until an input source exists.
Reading from an absent input is an error the program can observe, not a hang.

## The Console Server
The console server is the first consumer of this design: the program behind every ordinary output stream.
It holds the read ends of the streams the coordinator wires to it, multiplexes them through its [[IPC Primitives#Ports|port]], and drains their bytes to the actual output device.
Interleaving policy -- per-write atomicity, per-program prefixes, buffering -- lives entirely in this one program.

The coordinator spawns the console server like any other program, recognizes it by its image name, and mails it the read end of each subsequently spawned program's output and error streams, tagged with the writer's name.
Programs spawned before the console server exists have their read ends parked by the coordinator and delivered when it arrives, the same ordering-independence the [[Service Coordination#Names and Connections|connect layer]] already provides.

### The device trajectory
The console server owns policy from day one but not yet the device.
Until the kernel can hand a device to userspace, the server drains its collected bytes through the kernel's debug write, which already serializes with the kernel log on the shared UART.
The debug write thereby becomes the console server's private backend on the way to deletion, and no client knows: clients hold socket ends, and swapping the server's sink changes nothing above it.

True device ownership needs two kernel bricks that are their own milestones: mapping a device VMO into a user address space, and interrupt delivery to userspace via an [[Interrupt Model|interrupt object]].
When they land, the console server maps the UART and the kernel log becomes the second writer to a device it no longer owns -- the panic path keeps a raw fallback, because the kernel cannot depend on userspace to report userspace's death.
Input is the same picture mirrored: an interrupt-driven driver feeds bytes into socket ends that the console server routes to whichever program holds the readable input stream.

## The Kernel Log Is Not a Stream
The kernel log stays a separate, structured, kernel-internal facility.
It shares the UART with the console server's sink today, and that sharing -- not any object -- is the coupling; the log never flows through a socket and the console server never carries kernel diagnostics.

## Relationship to Other Subsystems
- [[IPC Primitives#Sockets]] -- the primitive; streams are sockets plus convention
- [[Service Coordination]] -- the coordinator wires every stream at spawn; the console server is found by image name, not by registered name
- [[Task Model#Bootstrap]] -- the stdio message rides the bootstrap channel every task is born holding
- [[Syscall Interface#Input and Output]] -- why there are no read/write syscalls behind stdio
- [[Interrupt Model]] -- the brick the input half and true device ownership wait on
