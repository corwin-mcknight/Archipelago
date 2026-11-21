# File Service

The userspace service named `files` exposes a root directory containing `boot`. The `/boot` tree is an immutable view of the initrd. Init moves the original read-only archive VMO to the designated bootstrap child after extracting the executable images. The child maps it read-only and validates and indexes it before registering. Init requires that registration on the child's mailbox, with a bounded wait. Missing or failed registration aborts bootstrap. See [[Initrd]] for the archive profile.

Each brokered connection is a root directory capability. `OPEN` returns a fresh channel for the selected node. An open channel grants authority over that node and, for a directory, lookup below it. Clients with a root capability resolve absolute user-facing paths from root. The service accepts relative paths of 1–255 printable ASCII bytes, with nonempty components and no backslash, `.`, or `..`. Archive entries must have explicit directory parents. The index supports up to 4096 archive entries.

## Protocol

Messages are at most 2048 bytes including the existing 16-byte `abi_message_header`. Fields are native endian and packed. Requests carry status zero and a nonzero transaction ID. Replies echo the opcode and transaction ID. Stable opcode, status, and kind constants live in `<fileservice/protocol.h>`. Requests carry zero handles. An operation error is a header-only reply with nonzero status.

| Operation | Request payload | Successful reply payload |
| --- | --- | --- |
| `OPEN` | `u32` flags, then relative path bytes | Empty payload, one file or directory channel |
| `STAT` | Empty | `u64` size (zero for a directory), `u32` kind, `u32` reserved zero |
| `READ` | `u64` offset, `u32` maximum bytes | Raw bytes, capped by the message budget and remaining file length. At or beyond EOF, or for a zero-length request, the payload is empty |
| `READ_DIR` | `u64` cursor | `u64` next cursor, `u32` end flag, then complete entries of `u16` name length, `u8` kind, and name bytes |

`OPEN` and `READ_DIR` require a directory. `READ` requires a file. The only defined open flag requests write access and returns `READ_ONLY` for existing nodes. Unknown operations and flags return `UNSUPPORTED`. Cursor zero starts a listing. A reply with end flag one finishes it and carries cursor zero. Other replies carry an opaque continuation for that directory. A cursor that does not identify a child of the relevant directory returns `INVALID_CURSOR`. Entries sort by filename bytes. The immutable index keeps subsequent pages consistent without a mutable listing position.

## Ordering and lifetime

Requests and replies are FIFO within each endpoint. Clients may pipeline requests and match results by transaction ID. An accepted channel send only enqueues a message. The server runs one request at a time per endpoint and retains at most one pending reply there. When the reply queue fills, it waits for that endpoint's writable signal and continues serving other endpoints. If an attempted send fails despite writability, the endpoint closes and any remaining resources are released. A failed handle transfer is never replayed.

The server caps live endpoints at 64. At capacity, opening a node returns `RESOURCE_EXHAUSTED`, and a newly brokered root connection closes immediately. Closing an endpoint abandons its outstanding requests and pending reply. A previously received child capability remains independent. A client that keeps an endpoint open after a timeout still drains late replies and closes any unused handles. Bad framing or unexpected handles close the affected endpoint. Ordinary operation errors preserve it.

Writable anonymous storage and executable-content snapshots are later increments of [#21](https://github.com/corwin-mcknight/Archipelago/issues/21).
