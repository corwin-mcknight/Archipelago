# Executable Loading
This architecture is implemented on x86_64 and riscv64.

## Goal
Remove executable ELF loading entirely from the kernel on x86_64 and riscv64. The kernel loads init exactly once through a boot-only path. Init has a minimal non-ELF boot image; there is no syscall for loading this format or any other executable format.

Init uses `lib/elf` to validate bootstrap executables and start the `sys/elf_loader` service from the initrd, then transfers its construction authority to that service. The ELF loader runs in userspace. It prepares memory mappings in a dormant task and asks the kernel to create its initial thread and start it. The kernel validates memory access and launch parameters, but does not interpret executable bytes. ELF metadata used to symbolize the kernel's own crashes is independent of executable loading.

## Authority
Creating, populating, and starting dormant tasks requires an explicit construction capability. Ordinary task handles do not convey construction authority. Init receives the initial authority and can endow the ELF loader with it. The loader does not endow the programs it creates with that capability.

Starting a task is a one-way transition. Construction and start serialize; after start, construction authority cannot change its mappings or start it a second time. The child receives its bootstrap channel and self-handles before its first instruction executes.

## Memory
Mappings may be read-only, read/write, or read/execute, never read/write/execute. Executable backing must not have writable aliases, including aliases retained by the loader. The implementation copies each constructed mapping into private backing to establish this invariant. This includes writable mappings: construction snapshots bytes rather than sharing the loader's temporary buffers.

Executable parsing, relocation, segment layout, stack placement, and format policy belong to userspace. The kernel checks address ranges, alignment, permissions, and the validity of the initial execution context.

## Lifetime
Each loading operation belongs to a loading thread. A thread may construct a dormant task without making it runnable. Abandoning construction or exiting that thread releases its unfinished task and resources; ordinary task-wide handle cleanup alone is insufficient for this rule.

Successful start transfers the child to independent scheduler ownership. Exiting the loading thread after that point does not kill the child. The service starts one worker thread per spawn and joins it before replying. Workers receive their own IPC buffers and explicitly pass them to the loader library; the CRT's main-thread staging pointer is not used by workers. The worker's pending address space is released by the reaper, even when observers retain a reference to the dead thread.

Routine parse, mapping, and start failures abort the unfinished construction and return an error. An unexpected worker fault terminates the loader service after reporting failure, so task-owned temporary handles and mappings are also reclaimed. Restart and supervision remain coordinator policy.

## Boot image
The installed `init.bin` starts with a 64-byte `ARCHINIT` header containing a version, machine tag, entry address, and the sizes of four fixed regions: RX text, R constants, RW data, and zero-filled RW storage. Region lengths are page-aligned, addresses are implicit from a fixed base, and the kernel supplies the boot stack. There are no segment tables, relocations, interpreters, or syscall-accessible loaders. The ELF link intermediate stays in the build directory for debugging.

The other userspace boot input is `initrd.tar`, an uncompressed ustar archive handed to init as an opaque read-only VMO. Init validates the archive and bootstrap set before launching `bootstrap/elf_loader.elf`, then asks that service to start the remaining `bootstrap/<service>.elf` entries. It copies each executable into a separate VMO at offset zero, unmaps the writable staging view, and restricts the handle to read access before loading. Program and data entries outside `bootstrap/` remain available for future file-service access. The kernel does not parse the archive. See [[Initrd]] and [[Service Coordination]].

## Construction interface
Bootstrap handles are ordered: self task, initial thread, task-bound ThreadFactory, and (for boot init only) TaskFactory.

A TaskFactory capability authorizes CREATE, MAP, START, and ABORT on the calling thread's single pending container. A separate ThreadFactory capability, endowed to every task and bound to its identity, authorizes creating threads in that task. It grants no task construction authority. No other thread can access the pending container. Invalid launch parameters leave it available for correction; an attempted launch with valid parameters consumes it even if allocating launch resources fails.

The former image-spawn syscall number is reserved and rejected. Ordinary task handles continue to support status, wait, and kill without granting construction authority. The loader returns each successful child's task handle and parent bootstrap endpoint to init, preserving coordinator routing.

## Completion criteria
- Init's installed boot image is non-ELF and its loader can run only once.
- No production kernel executable-loading path parses ELF or accepts a generic memory image.
- A userspace ELF loader starts the remaining programs using capability-gated task construction.
- Programs created by the loader receive no construction capability.
- Failed, abandoned, and concurrently attempted construction/start operations preserve ownership and W^X.
- Host tests cover pure validation; headless QEMU tests exercise bootstrap, loading, authority, cleanup, and execution on both architectures.
