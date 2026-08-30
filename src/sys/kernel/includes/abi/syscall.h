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
#define ABI_SYS_EXIT 0ull /* arg0 = exit status; the low 32 bits are recorded on the task */
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
            a message larger than either capacity is consumed and       \
            discarded (its handles closed) and the recv fails with      \
            ERR_TRUNCATED, so the next recv sees the next message. */

// The most handles one message can carry.
#define ABI_CHANNEL_MAX_MESSAGE_HANDLES 4ull

// Error returns a user program must branch on, as signed values of the negative band described at
// SYS_HANDLE_CLOSE. The full band is still kernel-internal; codes are installed here one by one
// as programs first need them, and the kernel static_asserts each against its internal value.
#define ABI_ERR_TRUNCATED (-12ll)   /* recv: the front message exceeded a capacity and was discarded */
#define ABI_ERR_WOULD_BLOCK (-13ll) /* nothing to take right now; wait on READABLE and retry */
#define ABI_ERR_PEER_CLOSED (-14ll) /* the opposite endpoint is gone and nothing is left to take */
#define ABI_ERR_TIMED_OUT (-15ll)   /* a bounded wait lapsed */

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

// Task lifecycle: the first type-specific handle operations -- both verify the handle names a task.
//
// Kill is asynchronous and unrefusable: it marks every thread of the task, forces blocked ones
// awake, and each exits at its next kernel boundary; the task never executes another user
// instruction after the mark is observed. Kill on an already-terminated task is a no-op.
//
// Status is readable once the task has fully terminated (TASK_SIGNAL_TERMINATED asserted); before
// that it returns the would_block error. The value packs the exit cause in the high 32 bits and,
// for a normal exit, the status word SYS_EXIT carried in the low 32.
#define ABI_SYS_TASK_KILL 15ull /* arg0 = task handle (needs the write right). Returns 0. */
#define ABI_SYS_TASK_STATUS                             \
    16ull /* arg0 = task handle (needs the read right). \
             Returns (cause << 32) | status. */

// Exit causes, as returned in the high 32 bits of SYS_TASK_STATUS.
#define ABI_TASK_EXIT_EXITED 0ull
#define ABI_TASK_EXIT_KILLED 1ull
#define ABI_TASK_EXIT_FAULTED 2ull

// User-controlled memory: anonymous VMOs a task creates, maps into its own address space, and
// unmaps. Sizes, offsets, and addresses are in bytes and must be multiples of VM_PAGE_SIZE --
// rejected when they are not, never rounded, so an unaligned argument is a caller bug surfaced
// rather than absorbed.
//
// Pages materialize on first touch (zero-filled), so create is cheap and memory is consumed as
// used. A mapping holds its own reference: closing the VMO handle does not unmap, and the memory
// lives until the last handle and the last mapping are both gone -- a heap may map, close the
// handle, and keep the memory.
#define ABI_VM_PAGE_SIZE 4096ull

// Mapping protections. Mapping READ requires the read right on the VMO handle, WRITE the write
// right. EXEC is installed ABI but every map requesting it is rejected -- user-minted executable
// mappings wait for the userspace-loader milestone, which is also what keeps W^X trivially true.
#define ABI_VM_PROT_READ (1ull << 0)
#define ABI_VM_PROT_WRITE (1ull << 1)
#define ABI_VM_PROT_EXEC (1ull << 2)

// Sockets: a bidirectional, point-to-point byte-stream pair -- the streaming counterpart to a
// channel (docs/Design/IPC Primitives.md). Bytes only: no message boundaries, no handle slots.
// A read returns whatever bytes are available up to the caller's capacity, independent of how
// they were written; a write appends as much as the peer's bounded buffer can take and returns
// the count, so a short write is backpressure, not an error. Only a write finding zero room
// fails (capacity_exhausted -- wait on WRITABLE and retry), and only a read finding zero bytes
// fails: would_block while the peer lives, peer_closed once it is gone. Bytes buffered when the
// peer closes remain readable; peer_closed is the empty-and-never-again condition.
//
// Direction is a property of the handle, not the object: an end without the write right is a
// read-only stream, so a pipe is this same pair with the opposing right stripped from each side.
// Data moves only through the IPC buffer, like every other syscall.
#define ABI_SYS_SOCKET_CREATE                                     \
    21ull /* arg0 = IPC-buffer offset where the kernel writes the \
             two endpoint handles as two uint64s. Returns 0. */
#define ABI_SYS_SOCKET_WRITE                                 \
    22ull /* arg0 = handle (needs the write right), arg1 =   \
             IPC-buffer offset, arg2 = length. Returns bytes \
             accepted, possibly short of length. */
#define ABI_SYS_SOCKET_READ                               \
    23ull /* arg0 = handle (needs the read right), arg1 = \
             IPC-buffer offset, arg2 = capacity. Returns bytes read. */

// Socket endpoint signals: the same shape as the channel bits. READABLE while bytes are
// buffered, WRITABLE while the peer's buffer has room, PEER_CLOSED once the opposite endpoint
// is destroyed.
#define ABI_SOCKET_SIGNAL_READABLE (1ull << 0)
#define ABI_SOCKET_SIGNAL_WRITABLE (1ull << 1)
#define ABI_SOCKET_SIGNAL_PEER_CLOSED (1ull << 2)

#define ABI_SYS_VMO_CREATE 18ull /* arg0 = size in bytes. Returns the new VMO handle. */
// Returns the mapped base address, which can never look negative: user addresses live in the low
// canonical half, so bit 63 of a mapped base is always clear.
#define ABI_SYS_VMO_MAP                                             \
    19ull /* arg0 = VMO handle (rights per prot, above), arg1 =     \
             address (0 = kernel picks; nonzero is honored exactly  \
             or fails on overlap), arg2 = byte offset into the VMO, \
             arg3 = length, arg4 = prot. Returns the mapped base. */
#define ABI_SYS_VMO_UNMAP                                     \
    20ull /* arg0 = any address inside a mapping. Removes the \
             whole mapping containing it. Returns 0. */

// Spawn a task from an executable image. Holding the image VMO is the whole authority -- there is
// no ambient spawn privilege and no kernel-side list of programs; images arrive as IMAGE messages
// (<abi/message.h>) or wherever else a VMO handle travels. The kernel parses the image (static
// ELF today; the loader is destined for userspace, see docs/Design/Service Coordination.md),
// builds the task, and queues its first thread. The caller becomes the parent: it receives the
// new task's handle and the parent end of its bootstrap channel, and everything the child ever
// learns beyond its self-handles arrives through that channel. Closing the channel end is how the
// child observes parent death (PEER_CLOSED). The image must be physically contiguous, which every
// kernel-minted image VMO is.
#define ABI_SYS_TASK_SPAWN                                        \
    17ull /* arg0 = image VMO handle (needs the read right),      \
             arg1 = IPC-buffer offset where the kernel writes the \
             new task handle then the bootstrap channel handle,   \
             two uint64s. Returns 0. */

// Signal bits, as returned and waited on through SYS_OBJECT_WAIT. Meanings are per object type;
// the channel bits are the first installed as ABI. The kernel manages all three: READABLE while
// the endpoint has queued messages, WRITABLE while the peer has queue room, PEER_CLOSED once the
// opposite endpoint is destroyed.
#define ABI_CHANNEL_SIGNAL_READABLE (1ull << 0)
#define ABI_CHANNEL_SIGNAL_WRITABLE (1ull << 1)
#define ABI_CHANNEL_SIGNAL_PEER_CLOSED (1ull << 2)

// Asserted while the port has pending packets.
#define ABI_PORT_SIGNAL_READABLE (1ull << 0)

// Asserted on a task object once teardown is complete: threads dead, handles closed, address
// space gone. Waitable and port-bindable like any signal; SYS_TASK_STATUS is valid from then on.
#define ABI_TASK_SIGNAL_TERMINATED (1ull << 0)

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
constexpr int64_t ERR_TRUNCATED                = ABI_ERR_TRUNCATED;
constexpr int64_t ERR_WOULD_BLOCK              = ABI_ERR_WOULD_BLOCK;
constexpr int64_t ERR_PEER_CLOSED              = ABI_ERR_PEER_CLOSED;
constexpr int64_t ERR_TIMED_OUT                = ABI_ERR_TIMED_OUT;
constexpr uint64_t SYS_OBJECT_WAIT             = ABI_SYS_OBJECT_WAIT;
constexpr uint64_t SYS_PORT_CREATE             = ABI_SYS_PORT_CREATE;
constexpr uint64_t SYS_PORT_BIND               = ABI_SYS_PORT_BIND;
constexpr uint64_t SYS_PORT_UNBIND             = ABI_SYS_PORT_UNBIND;
constexpr uint64_t SYS_PORT_WAIT               = ABI_SYS_PORT_WAIT;
constexpr uint64_t SYS_TASK_KILL               = ABI_SYS_TASK_KILL;
constexpr uint64_t SYS_TASK_STATUS             = ABI_SYS_TASK_STATUS;
constexpr uint64_t SYS_TASK_SPAWN              = ABI_SYS_TASK_SPAWN;
constexpr uint64_t TASK_EXIT_EXITED            = ABI_TASK_EXIT_EXITED;
constexpr uint64_t TASK_EXIT_KILLED            = ABI_TASK_EXIT_KILLED;
constexpr uint64_t TASK_EXIT_FAULTED           = ABI_TASK_EXIT_FAULTED;
constexpr uint64_t VM_PAGE_SIZE                = ABI_VM_PAGE_SIZE;
constexpr uint64_t VM_PROT_READ                = ABI_VM_PROT_READ;
constexpr uint64_t VM_PROT_WRITE               = ABI_VM_PROT_WRITE;
constexpr uint64_t VM_PROT_EXEC                = ABI_VM_PROT_EXEC;
constexpr uint64_t SYS_VMO_CREATE              = ABI_SYS_VMO_CREATE;
constexpr uint64_t SYS_VMO_MAP                 = ABI_SYS_VMO_MAP;
constexpr uint64_t SYS_VMO_UNMAP               = ABI_SYS_VMO_UNMAP;
constexpr uint64_t SYS_SOCKET_CREATE           = ABI_SYS_SOCKET_CREATE;
constexpr uint64_t SYS_SOCKET_WRITE            = ABI_SYS_SOCKET_WRITE;
constexpr uint64_t SYS_SOCKET_READ             = ABI_SYS_SOCKET_READ;
constexpr uint64_t SOCKET_SIGNAL_READABLE      = ABI_SOCKET_SIGNAL_READABLE;
constexpr uint64_t SOCKET_SIGNAL_WRITABLE      = ABI_SOCKET_SIGNAL_WRITABLE;
constexpr uint64_t SOCKET_SIGNAL_PEER_CLOSED   = ABI_SOCKET_SIGNAL_PEER_CLOSED;
constexpr uint64_t TASK_SIGNAL_TERMINATED      = ABI_TASK_SIGNAL_TERMINATED;
constexpr uint64_t CHANNEL_SIGNAL_READABLE     = ABI_CHANNEL_SIGNAL_READABLE;
constexpr uint64_t CHANNEL_SIGNAL_WRITABLE     = ABI_CHANNEL_SIGNAL_WRITABLE;
constexpr uint64_t CHANNEL_SIGNAL_PEER_CLOSED  = ABI_CHANNEL_SIGNAL_PEER_CLOSED;
constexpr uint64_t PORT_SIGNAL_READABLE        = ABI_PORT_SIGNAL_READABLE;
constexpr uint64_t BOOTSTRAP_HANDLE            = ABI_BOOTSTRAP_HANDLE;

// Syscall numbers form a dense table today. Keep additions ordered and prevent an accidental
// duplicate or gap from silently disagreeing with dispatch metadata and userspace stubs.
struct syscall_descriptor {
    uint64_t number;
    uint8_t argument_count;
};

constexpr syscall_descriptor SYSCALL_DESCRIPTORS[] = {
    {SYS_EXIT, 1},          {SYS_YIELD, 0},          {SYS_SLEEP, 1},
    {SYS_WRITE, 2},         {SYS_HANDLE_CLOSE, 1},   {SYS_HANDLE_DUPLICATE, 2},
    {SYS_OBJ_INFO, 1},      {SYS_CHANNEL_CREATE, 1}, {SYS_CHANNEL_SEND, 5},
    {SYS_CHANNEL_RECV, 5},  {SYS_OBJECT_WAIT, 3},    {SYS_PORT_CREATE, 0},
    {SYS_PORT_BIND, 4},     {SYS_PORT_UNBIND, 2},    {SYS_PORT_WAIT, 3},
    {SYS_TASK_KILL, 1},     {SYS_TASK_STATUS, 1},    {SYS_TASK_SPAWN, 2},
    {SYS_VMO_CREATE, 1},    {SYS_VMO_MAP, 5},        {SYS_VMO_UNMAP, 1},
    {SYS_SOCKET_CREATE, 1}, {SYS_SOCKET_WRITE, 3},   {SYS_SOCKET_READ, 3},
};

consteval bool syscall_descriptors_are_valid() {
    for (uint64_t i = 0; i < sizeof(SYSCALL_DESCRIPTORS) / sizeof(SYSCALL_DESCRIPTORS[0]); ++i) {
        if (SYSCALL_DESCRIPTORS[i].number != i || SYSCALL_DESCRIPTORS[i].argument_count > 6) { return false; }
    };
    return true;
}

static_assert(syscall_descriptors_are_valid(), "syscall metadata must remain dense, unique, and fit the entry ABI");

}  // namespace abi::syscall
#endif
