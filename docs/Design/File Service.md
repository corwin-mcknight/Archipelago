# File Service

The userspace service named `files` stores named nodes in memory for the current boot. Each writable node, including the root, can hold both bytes and named children. Closing every handle leaves its name, contents, and children available until the service exits. The `/boot` tree is an immutable view of the initrd. Init moves the original read-only archive VMO to the designated bootstrap child after extracting the executable images. The child maps it read-only and validates and indexes it before registering. Init requires that registration on the child's mailbox, with a bounded wait. Missing or failed registration aborts bootstrap. See [[Initrd]] for the archive profile.

## Names and authority

A brokered connection grants a full-access root handle under the coordinator’s open discovery policy. Clients resolve user-facing absolute paths relative to this handle. Both lookup paths and the full root-relative path of a created node are limited to 255 bytes. Paths use printable ASCII, with nonempty components and no backslash, `.`, or `..`. Create takes one component. Names are case-sensitive and exclusive within their parent. Creating `hello` twice returns `ALREADY_EXISTS` on the second attempt. Creating `Hello` adds a distinct sibling.

Each handle carries two masks. Its **rights** govern operations on this node. Its **descendant ceiling** limits both masks a caller can request when opening or creating a child. Lookup through several components also requires Lookup in the ceiling for intermediate steps. Two handles to the same node can have different rights.

| Right or mode | Permitted operation |
| --- | --- |
| Read-only | Read bytes by offset. |
| Write-only append | Add bytes at the current end. The server chooses the position when processing each request. |
| Read-write | Read, overwrite or extend from a specified offset, or append. |
| List | Page through child names. |
| Lookup | Open an existing child by name. |
| Create | Add a new child and receive its first handle with the requested rights and ceiling. |

List, Lookup, and Create are independent. A handle can have any combination of these, including with no byte access. For example, Create alone on a parent with an append-only ceiling lets a client create a child and append through its first handle. A separate reader with Lookup and a read-only ceiling can open that child and read it. Exceeding the ceiling returns `ACCESS_DENIED`. A create denied by the ceiling leaves the requested name available for a retry.

## Protocol

Messages are at most 2048 bytes including the existing 16-byte `abi_message_header`. Fields are native endian and packed. Requests carry status zero and a nonzero transaction ID. Replies echo the opcode and transaction ID. Stable opcode, status, and kind constants live in `<fileservice/protocol.h>`. Requests carry zero handles. An operation error is a header-only reply with nonzero status.

| Operation | Request payload | Successful reply payload |
| --- | --- | --- |
| `OPEN` | `u32` flags, then relative path bytes | Empty payload, one file or directory channel |
| `OPEN_RIGHTS` | `u32` rights, `u32` descendant ceiling, then relative path bytes | Empty payload, one node channel |
| `CREATE` | Same masks, then one child name | Empty payload, one new node channel |
| `WRITE` | `u64` offset followed by raw bytes | Empty payload after the entire write succeeds |
| `STAT` | Empty | `u64` byte size, `u32` kind, `u32` reserved zero |
| `READ` | `u64` offset, `u32` maximum bytes | Raw bytes, capped by the message budget and remaining file length. At or beyond EOF, or for a zero-length request, the payload is empty |
| `READ_DIR` | `u64` cursor | `u64` next cursor, `u32` end flag, then complete entries of `u16` name length, `u8` kind, and name bytes |

The original `OPEN` with zero flags requests read access, plus List and Lookup when the parent ceiling permits them. `WRITE_ACCESS` additionally requests read-write access. The resulting ceiling equals the granted rights. Clients use `OPEN_RIGHTS` to request independent masks. Unsupported byte-mode combinations, unknown rights bits, flags and operations return `UNSUPPORTED`. Every handle supports `STAT`. Writable nodes report the compatibility kind `DIRECTORY` when they have children and `FILE` when empty of children. Both kinds support byte and child operations subject to the handle rights. The initrd subtree retains its archive kinds and read-only behavior, including `WRONG_KIND` for a byte read on an archive directory and `READ_ONLY` for a write or create.

`WRITE` carries up to 2024 bytes. The reserved offset `APPEND_OFFSET` selects append. An append-only handle rejects a positional write. Positional writes preserve bytes outside their range, can extend at EOF, and reject a gap with `INVALID_OFFSET`. An acknowledged write is visible to a subsequent read through another handle. Storage supports 256 created nodes and 1 MiB of writable contents across the tree, in addition to up to 4096 archive entries. Capacity and allocation failures return `RESOURCE_EXHAUSTED` and preserve the earlier contents.

A `READ_DIR` cursor of zero starts a listing. Entries sort by filename bytes. Each nonfinal page returns an opaque cursor identifying the next child. A cursor outside the relevant parent returns `INVALID_CURSOR`. The final page has end flag one and cursor zero. A listing of a mutable node is live across pages. Children inserted before a saved continuation are skipped on resume, and a fresh listing includes them.

## Ordering and lifetime

Requests and replies are FIFO within each endpoint. Clients may pipeline requests and match results by transaction ID. An accepted channel send only enqueues a message. The server runs one request at a time per endpoint and retains at most one pending reply there. When the reply queue fills, it waits for that endpoint's writable signal and continues serving other endpoints. If an attempted send fails despite writability, the endpoint closes and any remaining resources are released. A failed handle transfer is never replayed.

The server caps live endpoints at 64. At capacity, opening a node returns `RESOURCE_EXHAUSTED`, and a newly brokered root connection closes immediately. Closing an endpoint abandons its outstanding requests and pending reply. A previously received child capability remains independent. A client that keeps an endpoint open after a timeout still drains late replies and closes any unused handles. Bad framing or unexpected handles close the affected endpoint. Ordinary operation errors preserve it.

The two-client demo runs in the guest with `make test-verbose ARCH=x86_64 TEST=coordinator_boot` or `ARCH=riscv64`. The writer checks that `hello` is absent in the fresh service, creates it, appends `Hello, world!\n`, closes its handles, and signals the reader through the coordinator. The reader opens `hello` from its own root and verifies all 14 bytes. Executable-content snapshots remain a later increment of [#21](https://github.com/corwin-mcknight/Archipelago/issues/21).
