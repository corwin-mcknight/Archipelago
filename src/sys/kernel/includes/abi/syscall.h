#pragma once

#include <stdint.h>

// The user/kernel syscall contract. Public: installed to /usr/include by sys/kernel-headers and
// compiled into both the kernel and every user program, so the two cannot drift. Nothing
// kernel-internal belongs here -- see <kernel/syscall.h> for the dispatch side.

namespace abi::syscall {

// The syscall number names the operation; a handle argument names the object it acts on.
constexpr uint64_t SYS_EXIT                    = 0;
constexpr uint64_t SYS_YIELD                   = 1;
constexpr uint64_t SYS_SLEEP                   = 2;  // arg0 = kernel ticks
// arg0 = offset into this thread's IPC buffer, arg1 = byte count. Returns bytes written, or a
// negative ktl::errc. No pointer crosses the boundary: the buffer is the only memory a syscall
// reads from the caller, and the kernel already knows where its pages are.
constexpr uint64_t SYS_WRITE                   = 3;

// Handle operations. Every one of them takes a handle in arg0 and runs the same verification
// pipeline before any operation code executes: slot-and-generation lookup in the calling task's
// handle table, then a type check, then a rights check. A failure at any step returns a negative
// error with nothing done.
//
// A handle is a uint64: table slot index in the low 32 bits, slot generation in the high 32.
// Reusing a closed handle fails the generation check -- a recycled slot has a new generation.
//
// Error returns occupy the small negative band (roughly -1 .. -64). A successful
// SYS_HANDLE_DUPLICATE returns the new handle, which can never look negative: the kernel retires
// a slot when its generation reaches 2^31 - 1, so bit 63 of a packed handle is always clear, and
// a slot index would have to exceed 0xFFFFFFC0 -- four billion live handles -- to reach the band.
constexpr uint64_t SYS_HANDLE_CLOSE            = 4;  // arg0 = handle. Returns 0.
constexpr uint64_t SYS_HANDLE_DUPLICATE        = 5;  // arg0 = handle (needs the duplicate right),
                                                     // arg1 = rights mask. Returns the new handle.
constexpr uint64_t SYS_OBJ_INFO                = 6;  // arg0 = handle. Returns type id in the low 32 bits,
                                                     // the handle's rights in the high 32.

// Channels: a bidirectional message pair. Data still moves only through the IPC buffer -- send
// reads the message out of it, recv writes the message into it, and create delivers the two new
// handles through it, the first kernel-to-user copy-out.
//
// Sends never block: a full peer queue fails immediately and the caller decides whether to wait,
// retry, or drop. Recv likewise returns immediately when the queue is empty.
//
// Messages can carry handles. Send takes them as uint64 handle values in the IPC buffer at arg3,
// arg4 many, at most CHANNEL_MAX_MESSAGE_HANDLES; sending any requires the transfer right on the
// channel handle. Each is removed from the sender's table and rides the message; the receiver's
// recv names where arrived handles land (arg3) and how many it has room for (arg4). A handle in
// transit lives in the kernel's own table, so a channel that dies with messages queued closes
// them like any other handle. Once a send has begun consuming handles they belong to the message:
// a send that fails partway closes what it took, and a message dropped unread closes what it
// carried. The transferred handle arrives with a new value but the same rights.
constexpr uint64_t SYS_CHANNEL_CREATE          = 7;  // arg0 = IPC-buffer offset where the kernel writes
                                                     // the two endpoint handles as two uint64s. Returns 0.
constexpr uint64_t SYS_CHANNEL_SEND            = 8;  // arg0 = handle (needs the write right), arg1 = IPC-
                                                     // buffer offset, arg2 = length, arg3 = IPC-buffer
                                                     // offset of handles to send (needs the transfer
                                                     // right), arg4 = handle count. Returns 0.
constexpr uint64_t SYS_CHANNEL_RECV            = 9;  // arg0 = handle (needs the read right), arg1 = IPC-
                                                     // buffer offset, arg2 = capacity, arg3 = IPC-buffer
                                                     // offset for arrived handles, arg4 = handle
                                                     // capacity. Returns the handle count in the high 32
                                                     // bits and the byte count in the low 32; a message
                                                     // larger than either capacity fails and stays queued.

// The most handles one message can carry.
constexpr uint64_t CHANNEL_MAX_MESSAGE_HANDLES = 4;

// arg0 = handle (needs the wait right), arg1 = signal mask. A nonzero mask blocks the calling
// thread until any bit in it is asserted on the object and returns the signals observed at wake; a
// zero mask is a poll, returning the current signals immediately (possibly zero). Signals are 32
// bits; a mask with any higher bit set is rejected. No timeout exists yet -- a wait whose signal
// never fires blocks forever.
constexpr uint64_t SYS_OBJECT_WAIT             = 10;

// Signal bits, as returned and waited on through SYS_OBJECT_WAIT. Meanings are per object type;
// the channel bits are the first installed as ABI. The kernel manages all three: READABLE while
// the endpoint has queued messages, WRITABLE while the peer has queue room, PEER_CLOSED once the
// opposite endpoint is destroyed.
constexpr uint64_t CHANNEL_SIGNAL_READABLE     = 1 << 0;
constexpr uint64_t CHANNEL_SIGNAL_WRITABLE     = 1 << 1;
constexpr uint64_t CHANNEL_SIGNAL_PEER_CLOSED  = 1 << 2;

// The initial thread's handle table is created with exactly two entries, in this order: a handle
// to its own task, then a handle to its own thread. Both are first-generation, so their packed
// values are the slot indices themselves.
constexpr uint64_t SELF_TASK_HANDLE            = 0;
constexpr uint64_t SELF_THREAD_HANDLE          = 1;

}  // namespace abi::syscall
