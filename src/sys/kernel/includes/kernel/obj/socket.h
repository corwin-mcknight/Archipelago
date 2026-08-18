#pragma once

#include <abi/syscall.h>
#include <kernel/obj/object.h>
#include <kernel/obj/type_registry.h>
#include <kernel/obj/types.h>

#include <ktl/ref>
#include <ktl/result>

namespace kernel::obj {

struct socket_state;

// One endpoint of a bidirectional, point-to-point byte stream (docs/Design/IPC Primitives.md) --
// the streaming counterpart to Channel. Bytes only: no message boundaries, no handle slots, no
// per-message queue accounting; a stream of text pays for none of what protocol messages need.
// Direction is a property of the handle, not the object: an endpoint handle without the write
// right is a read-only stream, so one primitive covers both socketpair and pipe shapes.
// Every operation fails or shortens rather than blocking; callers park on the signal bits.
class Socket : public Object {
   public:
    DECLARE_OBJECT_TYPE(Socket, type_ids::SOCKET)

    // Per-direction buffer: one page, allocated at create and owned by the pair's shared state.
    static constexpr size_t BUFFER_BYTES         = 4096;

    static constexpr uint32_t SIGNAL_READABLE    = static_cast<uint32_t>(::abi::syscall::SOCKET_SIGNAL_READABLE);
    static constexpr uint32_t SIGNAL_WRITABLE    = static_cast<uint32_t>(::abi::syscall::SOCKET_SIGNAL_WRITABLE);
    static constexpr uint32_t SIGNAL_PEER_CLOSED = static_cast<uint32_t>(::abi::syscall::SOCKET_SIGNAL_PEER_CLOSED);

    // Move-only handles, like Channel and for the same reason: PEER_CLOSED fires on the last
    // close, so duplicated ends would make hangup unreadable. No TRANSFER either -- carrying a
    // socket end in a message is gated by the channel handle it rides, not by this handle.
    static constexpr Rights DEFAULT_RIGHTS       = RIGHT_READ | RIGHT_WRITE | RIGHT_WAIT;

    struct Pair {
        ktl::ref<Socket> first;
        ktl::ref<Socket> second;
    };
    static ktl::result<Pair> create();

    ~Socket() override;

    // Append up to `length` bytes to the peer's buffer, returning how many fit -- a short count
    // is backpressure, not an error. capacity_exhausted only when nothing fits (wait on WRITABLE
    // and retry); peer_closed once the opposite endpoint is gone.
    ktl::result<size_t> write(const void* data, size_t length);

    // Take up to `capacity` buffered bytes, independent of how they were written. would_block
    // when empty with a live peer; peer_closed when empty for good. Bytes buffered when the peer
    // closed remain readable first.
    ktl::result<size_t> read(void* out, size_t capacity);

    static ktl::result<void> register_type(TypeRegistry& registry) {
        return registry.register_type(TYPE_ID, "socket", DEFAULT_RIGHTS, DEFAULT_RIGHTS);
    }

    // Use create(); public only because make_ref needs it.
    Socket(ktl::ref<socket_state> state, uint32_t side);

   private:
    ktl::ref<socket_state> m_state;
    uint32_t m_side;
};

}  // namespace kernel::obj
