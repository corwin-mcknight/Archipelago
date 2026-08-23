#include <kernel/mm/object_arena.h>
#include <kernel/obj/channel.h>
#include <kernel/obj/socket.h>
#include <kernel/synchronization/guard.h>
#include <kernel/synchronization/mutex.h>
#include <std/new.h>

namespace kernel::obj {

// The pair's shared half, mirroring channel_state: one lock covering both directions, raw
// back-pointers nulled as each endpoint dies, and one byte ring per direction indexed by the
// side that reads it. Ring pages come from the same one-page PMM helper the channels use.
struct socket_state {
    static void* operator new(size_t size, const std::nothrow_t&) noexcept;
    static void operator delete(void* ptr);

    kernel::synchronization::mutex lock;
    Socket* ends[2] = {nullptr, nullptr};

    struct byte_ring {
        uintptr_t page = 0;
        size_t head    = 0;
        size_t count   = 0;
    };
    byte_ring inbound[2];

    ~socket_state() {
        for (auto& ring : inbound) {
            if (ring.page != 0) { channel_page_free(ring.page); }
        }
    }
};

[[clang::no_destroy]] static kernel::mm::object_arena g_socket_state_arena("socket-state", sizeof(socket_state),
                                                                           alignof(socket_state));

void* socket_state::operator new(size_t size, const std::nothrow_t&) noexcept {
    return size <= sizeof(socket_state) ? g_socket_state_arena.alloc() : nullptr;
}
void socket_state::operator delete(void* ptr) { g_socket_state_arena.free(ptr); }

static_assert(Socket::BUFFER_BYTES <= 4096, "one ring per direction fits its one page");

Socket::Socket(ktl::ref<socket_state> state, uint32_t side)
    : Object(TYPE_ID), m_state(ktl::move(state)), m_side(side) {}

ktl::result<Socket::Pair> Socket::create() {
    auto state = ktl::make_ref<socket_state>();
    if (!state) { return ktl::err(ktl::errc::oom); }
    for (auto& ring : state->inbound) {
        ring.page = channel_page_alloc();
        if (ring.page == 0) { return ktl::err(ktl::errc::oom); }  // ~socket_state frees the other
    }
    auto first  = ktl::make_ref<Socket>(state, 0u);
    auto second = ktl::make_ref<Socket>(state, 1u);
    if (!first || !second) { return ktl::err(ktl::errc::oom); }

    // No lock: nothing can reach the pair before create() returns it.
    state->ends[0] = first.get();
    state->ends[1] = second.get();
    first->signal_set(SIGNAL_WRITABLE);
    second->signal_set(SIGNAL_WRITABLE);
    return ktl::result<Pair>::ok(Pair{ktl::move(first), ktl::move(second)});
}

Socket::~Socket() {
    kernel::synchronization::lock_guard guard(m_state->lock);
    m_state->ends[m_side] = nullptr;
    // Buffered bytes stay readable on the peer, so its READABLE is left alone.
    if (Socket* peer = m_state->ends[m_side ^ 1]) {
        peer->signal_clear(SIGNAL_WRITABLE);
        peer->signal_set(SIGNAL_PEER_CLOSED);
    }
}

ktl::result<size_t> Socket::write(const void* data, size_t length) {
    kernel::synchronization::lock_guard guard(m_state->lock);
    Socket* peer = m_state->ends[m_side ^ 1];
    if (!peer) { return ktl::err(ktl::errc::peer_closed); }
    if (length == 0) { return ktl::result<size_t>::ok(0); }

    auto& ring   = m_state->inbound[m_side ^ 1];
    size_t space = BUFFER_BYTES - ring.count;
    if (space == 0) { return ktl::err(ktl::errc::capacity_exhausted); }

    // At most two runs: tail-to-edge, then wrap to the front.
    size_t take        = length < space ? length : space;
    const uint8_t* src = static_cast<const uint8_t*>(data);
    size_t tail        = (ring.head + ring.count) % BUFFER_BYTES;
    size_t first_run   = take < BUFFER_BYTES - tail ? take : BUFFER_BYTES - tail;
    __builtin_memcpy(reinterpret_cast<uint8_t*>(ring.page) + tail, src, first_run);
    __builtin_memcpy(reinterpret_cast<uint8_t*>(ring.page), src + first_run, take - first_run);
    ring.count += take;

    peer->signal_set(SIGNAL_READABLE);
    if (ring.count == BUFFER_BYTES) { signal_clear(SIGNAL_WRITABLE); }
    return ktl::result<size_t>::ok(take);
}

ktl::result<size_t> Socket::read(void* out, size_t capacity) {
    kernel::synchronization::lock_guard guard(m_state->lock);
    auto& ring = m_state->inbound[m_side];
    if (ring.count == 0) {
        bool peer_alive = m_state->ends[m_side ^ 1] != nullptr;
        return ktl::err(peer_alive ? ktl::errc::would_block : ktl::errc::peer_closed);
    }
    if (capacity == 0) { return ktl::result<size_t>::ok(0); }

    size_t take      = capacity < ring.count ? capacity : ring.count;
    uint8_t* dst     = static_cast<uint8_t*>(out);
    size_t first_run = take < BUFFER_BYTES - ring.head ? take : BUFFER_BYTES - ring.head;
    __builtin_memcpy(dst, reinterpret_cast<const uint8_t*>(ring.page) + ring.head, first_run);
    __builtin_memcpy(dst + first_run, reinterpret_cast<const uint8_t*>(ring.page), take - first_run);
    bool was_full = ring.count == BUFFER_BYTES;
    ring.head     = (ring.head + take) % BUFFER_BYTES;
    ring.count -= take;

    if (ring.count == 0) { signal_clear(SIGNAL_READABLE); }
    if (was_full) {
        if (Socket* peer = m_state->ends[m_side ^ 1]) { peer->signal_set(SIGNAL_WRITABLE); }
    }
    return ktl::result<size_t>::ok(take);
}

}  // namespace kernel::obj
