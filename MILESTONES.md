# Milestones
System-level outcomes for Archipelago, grouped by status. Implementation tasks and dependencies live in [todo.md](todo.md).


## In Progress
No milestones currently tracked in this stage.

## Upcoming

### Milestone 2 -- Useful Work in Userspace
Given an init executable and an initrd supplied by Limine, Archipelago boots into an interactive userspace shell. The shell can launch programs stored in the initrd, and those programs can exchange data through files in a writable, memory-backed filesystem. The complete path works on x86_64 and riscv64.

#### System responsibilities
| Component | Responsibility |
|-----------|----------------|
| Kernel | Load the initial init ELF, hand init the opaque in-memory initrd blob and bootstrap capabilities, and provide primitives for constructing and starting tasks. Enforce memory and capability boundaries. |
| Init | Read enough of the initrd format to find bootstrap servers, load their ELF executables in userspace, and initialize the service environment. Construct and start subsequent programs through kernel primitives. |
| File server | Own file, directory, and namespace semantics. Provide a writable anonymous filesystem at `/` and expose the read-only initrd at `/boot`. |
| Shell | Interact with the file server to browse files and obtain executable contents, then ask init to execute those contents with arguments and standard streams. |

The path structure is Unix-like, while file access is implemented through userspace services and capabilities. The kernel has no VFS and does not interpret files, paths, or the initrd format. Its built-in ELF loader is used to start init; subsequent executable loading belongs to init.

Anonymous storage is a feature of the file server. Its contents are writable and last only for the current boot. The initrd remains a separate, read-only tree at `/boot`.

#### Boot and execution flow
1. Limine supplies the init executable and initrd as the two userspace boot inputs.
2. The kernel loads init and endows it with the initrd blob and the capabilities needed to bootstrap userspace.
3. Init locates the initial servers inside the blob and uses its own ELF loader and kernel task-building primitives to start them.
4. The file server establishes the writable root and read-only `/boot` tree; init establishes the console streams and launches the shell from the initrd through file access.
5. The shell obtains a requested executable through the file server and passes its contents to init for loading and execution.
6. Programs use the file server for data access and standard streams for interactive input and output.

Task construction must allow init to prepare memory mappings, the initial thread, and bootstrap endowments before the program begins execution. The kernel enforces mapping authority and executable-memory protections independently of the userspace loader.

#### Completion demonstration
- Boot to a userspace shell using only init and the initrd as userspace boot inputs; the file server, shell, and demonstration programs are contained in the initrd.
- Browse `/boot` and read a supplied data file through the file server.
- From the shell, launch a program from `/boot` that creates and writes a file in the writable root filesystem.
- After that program exits, launch a separate program that reads the created file and prints its contents, then returns control to the shell.
- Reject writes to `/boot`; report missing files and invalid executables without losing the shell.
- Repeat the demonstration on x86_64 and riscv64. After reboot, anonymous files are gone and the supplied `/boot` contents remain available.

This milestone establishes that Archipelago can perform a useful task in userspace. Subsequent milestones can build on this environment to make the system do more useful things.

### Milestone 3 -- Ethernet and IPv4 Networking in Userspace
Archipelago starts a userspace network driver and network stack, establishes a configured Ethernet/IPv4 connection, responds to incoming pings, and runs a userspace ping program that reaches an external IPv4 host.

#### System responsibilities
| Component | Responsibility |
|-----------|----------------|
| Kernel | Enforce access to hardware resources and provide the mappings, interrupt delivery, and safe DMA support needed by the network driver. |
| Init | Start the driver and network services, delegate their capabilities, and coordinate their lifecycle. |
| Network driver | Discover and configure the supported network adapter and send and receive Ethernet frames from userspace. |
| Network stack | Implement Ethernet, address resolution, IPv4, ICMP, and network configuration in userspace; expose network access to applications through userspace services. |
| Ping program | Use the configured network service to send echo requests to an IPv4 address and report replies or failure. |

The kernel does not interpret network packets or connections. This milestone establishes the userspace driver model through a complete hardware-backed capability: device discovery, resource delegation, interrupts, service access, and driver lifecycle. DMA-capable hardware requires an explicit isolation and teardown contract.

#### Completion demonstration
- Boot and initialize the network driver, stack, and connection through init.
- Ping Archipelago from another machine and receive replies.
- Launch ping from the userspace shell and receive replies from a reachable internet IPv4 address through the configured connection.
- Stop and restart the network driver and restore connectivity while the shell and file services remain usable.

Ethernet, IPv4, and ICMP define the networking scope. TCP and a small HTTP server are the next prospective consumer: an application that combines file access from Milestone 2 with network access from Milestone 3.

### Milestone 4 -- Persistent Storage and User Accounts
Archipelago becomes a persistent, personal system. Users can log in, save files and settings across reboots, and maintain separate private environments. Selected resources can be shared explicitly.

Storage drivers and file services provide persistence. Accounts, authentication, and sessions remain userspace responsibilities: trusted services decide which capabilities to grant, and the kernel enforces handles, rights, and isolation. Each application receives selected authority within its session.

This establishes the personal storage and session model that the later desktop will use. Device selection, filesystem format, authentication methods, and detailed sharing and logout contracts remain future design work.

#### Completion demonstration
- Log in, create a file, reboot, and log in again to read it back with saved settings intact.
- Use two accounts with separate private files and settings.
- Explicitly share a selected resource between accounts.
- Log out and switch accounts without exposing the previous session's private resources.

## Completed

### Milestone 1 -- User-mode Hello World
Boot-image ELF binaries run in user mode on x86_64 and riscv64, write to the console, and exit cleanly while the kernel shell remains available.

| Component | Accomplished with |
|-----------|------------------|
| Virtual memory | Per-task page tables and permissioned VMO mappings |
| Task object | Owned address space, handle table, and threads |
| Thread object | Saved context, kernel stack, and task/thread self-handles |
| ELF loader | Static ET_EXEC parsing, segment VMOs, ELF entry point |
| Syscall entry | x86 SYSCALL/SYSRET; riscv64 ECALL/SRET; checked handle dispatch |
| Context switching | Saved registers, trap frames, and per-thread FPU state |
| Serial output | SYS_WRITE through per-thread IPC buffers and kernel logging |
| Scheduler | Round-robin rotation on timer ticks and yield |
| Task termination | Exit status, TERMINATED signal, and reaper cleanup |
