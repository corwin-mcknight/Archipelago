#pragma once

#include <stdint.h>

// The user/kernel syscall contract. Public: installed to /usr/include by sys/kernel-headers and
// compiled into both the kernel and every user program, so the two cannot drift. Nothing
// kernel-internal belongs here -- see <kernel/syscall.h> for the dispatch side.
//
// The contract is spelled as macros so it reads from C and C++ alike; the kernel's C++ spelling
// of the same names lives in the namespace at the bottom, each alias defined by its macro so the
// two forms cannot diverge.

// The syscall number names the operation; a handle argument names the object it acts on.
#define ABI_SYS_EXIT 0ull
#define ABI_SYS_YIELD 1ull
#define ABI_SYS_SLEEP 2ull /* arg0 = kernel ticks */
// arg0 = offset into this thread's IPC buffer, arg1 = byte count. Returns bytes written, or a
// negative error. No pointer crosses the boundary: the buffer is the only memory a syscall
// reads from the caller, and the kernel already knows where its pages are.
#define ABI_SYS_WRITE 3ull

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
#define ABI_SYS_HANDLE_CLOSE 4ull /* arg0 = handle. Returns 0. */
#define ABI_SYS_HANDLE_DUPLICATE                       \
    5ull /* arg0 = handle (needs the duplicate right), \
            arg1 = rights mask. Returns the new handle. */
#define ABI_SYS_OBJ_INFO                                       \
    6ull /* arg0 = handle. Returns type id in the low 32 bits, \
            the handle's rights in the high 32. */

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
#define ABI_SYS_CHANNEL_CREATE                                       \
    7ull /* arg0 = IPC-buffer offset where the kernel writes the two \
            endpoint handles as two uint64s. Returns 0. */
#define ABI_SYS_CHANNEL_SEND                                           \
    8ull /* arg0 = handle (needs the write right), arg1 = IPC-buffer   \
            offset, arg2 = length, arg3 = IPC-buffer offset of handles \
            to send (needs the transfer right), arg4 = handle count.   \
            Returns 0. */
#define ABI_SYS_CHANNEL_RECV                                            \
    9ull /* arg0 = handle (needs the read right), arg1 = IPC-buffer     \
            offset, arg2 = capacity, arg3 = IPC-buffer offset for       \
            arrived handles, arg4 = handle capacity. Returns the handle \
            count in the high 32 bits and the byte count in the low 32; \
            a message larger than either capacity fails and stays       \
            queued. */

// The most handles one message can carry.
#define ABI_CHANNEL_MAX_MESSAGE_HANDLES 4ull

// arg0 = handle (needs the wait right), arg1 = signal mask, arg2 = timeout in nanoseconds. A
// nonzero mask blocks the calling thread until any bit in it is asserted on the object and returns
// the signals observed at wake; a zero mask is a poll, returning the current signals immediately
// (possibly zero, timeout ignored). Signals are 32 bits; a mask with any higher bit set is
// rejected. A timeout of 0 waits forever; a nonzero timeout is a floor, rounded up to the kernel's
// tick granularity, after which the wait returns the timed_out error instead of signals.
#define ABI_SYS_OBJECT_WAIT 10ull

// Ports: a multi-producer, single-consumer packet queue aggregating signal transitions from many
// objects into one waitable point. Bind subscribes an object's signals under a mask together with
// a caller-chosen 64-bit key; when the object's signal state transitions into the mask (or is
// already in it at bind), a packet queues on the port. Repeats before the packet is dequeued
// coalesce into it. A binding holds the object alive until unbound; unbinding a key drops its
// bindings and any pending packets. A dequeued packet is 16 bytes in the IPC buffer: the binding's
// key, then the accumulated signal bits, each as a uint64.
#define ABI_SYS_PORT_CREATE 11ull /* Returns the new port handle. */
#define ABI_SYS_PORT_BIND                                 \
    12ull /* arg0 = port handle (needs the write right),  \
             arg1 = object handle (needs the wait right), \
             arg2 = key, arg3 = signal mask. Returns 0. */
#define ABI_SYS_PORT_UNBIND                              \
    13ull /* arg0 = port handle (needs the write right), \
             arg1 = key. Returns how many bindings dropped. */
#define ABI_SYS_PORT_WAIT                                   \
    14ull /* arg0 = port handle (needs read + wait rights), \
             arg1 = IPC-buffer offset for the packet,       \
             arg2 = timeout ns (0 = forever). Returns 0, or timed_out. */

// Signal bits, as returned and waited on through SYS_OBJECT_WAIT. Meanings are per object type;
// the channel bits are the first installed as ABI. The kernel manages all three: READABLE while
// the endpoint has queued messages, WRITABLE while the peer has queue room, PEER_CLOSED once the
// opposite endpoint is destroyed.
#define ABI_CHANNEL_SIGNAL_READABLE (1ull << 0)
#define ABI_CHANNEL_SIGNAL_WRITABLE (1ull << 1)
#define ABI_CHANNEL_SIGNAL_PEER_CLOSED (1ull << 2)

// Asserted while the port has pending packets.
#define ABI_PORT_SIGNAL_READABLE (1ull << 0)

// The initial thread's handle table is created with exactly one entry: a channel endpoint,
// first-generation in slot 0, so its packed value is 0. The peer end belongs to whoever created
// the task. The first message on it is the bootstrap message: an empty byte payload carrying, in
// this order, a handle to the task itself, a handle to its initial thread, and then any further
// handles the creator chose to endow it with. Everything after that first message is ordinary
// parent-to-task mail, and the endpoint stays open for the task's life.
#define ABI_BOOTSTRAP_HANDLE 0ull

#ifdef __cplusplus
namespace abi::syscall {

constexpr uint64_t SYS_EXIT                    = ABI_SYS_EXIT;
constexpr uint64_t SYS_YIELD                   = ABI_SYS_YIELD;
constexpr uint64_t SYS_SLEEP                   = ABI_SYS_SLEEP;
constexpr uint64_t SYS_WRITE                   = ABI_SYS_WRITE;
constexpr uint64_t SYS_HANDLE_CLOSE            = ABI_SYS_HANDLE_CLOSE;
constexpr uint64_t SYS_HANDLE_DUPLICATE        = ABI_SYS_HANDLE_DUPLICATE;
constexpr uint64_t SYS_OBJ_INFO                = ABI_SYS_OBJ_INFO;
constexpr uint64_t SYS_CHANNEL_CREATE          = ABI_SYS_CHANNEL_CREATE;
constexpr uint64_t SYS_CHANNEL_SEND            = ABI_SYS_CHANNEL_SEND;
constexpr uint64_t SYS_CHANNEL_RECV            = ABI_SYS_CHANNEL_RECV;
constexpr uint64_t CHANNEL_MAX_MESSAGE_HANDLES = ABI_CHANNEL_MAX_MESSAGE_HANDLES;
constexpr uint64_t SYS_OBJECT_WAIT             = ABI_SYS_OBJECT_WAIT;
constexpr uint64_t SYS_PORT_CREATE             = ABI_SYS_PORT_CREATE;
constexpr uint64_t SYS_PORT_BIND               = ABI_SYS_PORT_BIND;
constexpr uint64_t SYS_PORT_UNBIND             = ABI_SYS_PORT_UNBIND;
constexpr uint64_t SYS_PORT_WAIT               = ABI_SYS_PORT_WAIT;
constexpr uint64_t CHANNEL_SIGNAL_READABLE     = ABI_CHANNEL_SIGNAL_READABLE;
constexpr uint64_t CHANNEL_SIGNAL_WRITABLE     = ABI_CHANNEL_SIGNAL_WRITABLE;
constexpr uint64_t CHANNEL_SIGNAL_PEER_CLOSED  = ABI_CHANNEL_SIGNAL_PEER_CLOSED;
constexpr uint64_t PORT_SIGNAL_READABLE        = ABI_PORT_SIGNAL_READABLE;
constexpr uint64_t BOOTSTRAP_HANDLE            = ABI_BOOTSTRAP_HANDLE;

}  // namespace abi::syscall
#endif
