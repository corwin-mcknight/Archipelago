#include <kernel/obj/channel.h>
#include <kernel/synchronization/guard.h>
#include <kernel/synchronization/mutex.h>

namespace kernel::obj {

// The pair's shared half: one lock covering both directions, raw back-pointers to the endpoints
// (nulled as each dies, so a live pointer here is always safe to signal), and the two message
// queues indexed by the side that reads them. Refcounted separately from the endpoints so neither
// endpoint keeps the other alive -- only this state outlives the first close.
struct channel_state {
    kernel::synchronization::mutex lock;
    Channel* ends[2] = {nullptr, nullptr};

    struct message_queue {
        MessageBuffer slots[Channel::QUEUE_DEPTH];
        size_t head  = 0;
        size_t count = 0;
    };
    message_queue inbound[2];
};

ktl::result<MessageBuffer> MessageBuffer::create(size_t length) {
    if (length > Channel::MAX_MESSAGE_BYTES) { return ktl::err(ktl::errc::out_of_range); }
    MessageBuffer buffer;
    if (length != 0) {
        buffer.m_page = channel_page_alloc();
        if (buffer.m_page == 0) { return ktl::err(ktl::errc::oom); }
    }
    buffer.m_length = length;
    return ktl::result<MessageBuffer>::ok(ktl::move(buffer));
}

Channel::Channel(ktl::ref<channel_state> state, uint32_t side)
    : Object(TYPE_ID), m_state(ktl::move(state)), m_side(side) {}

ktl::result<Channel::Pair> Channel::create() {
    auto state = ktl::make_ref<channel_state>();
    if (!state) { return ktl::err(ktl::errc::oom); }
    auto first  = ktl::make_ref<Channel>(state, 0u);
    auto second = ktl::make_ref<Channel>(state, 1u);
    if (!first || !second) { return ktl::err(ktl::errc::oom); }

    // No lock: nothing can reach the pair before create() returns it.
    state->ends[0] = first.get();
    state->ends[1] = second.get();
    first->signal_set(SIGNAL_WRITABLE);
    second->signal_set(SIGNAL_WRITABLE);
    return ktl::result<Pair>::ok(Pair{ktl::move(first), ktl::move(second)});
}

Channel::~Channel() {
    kernel::synchronization::lock_guard guard(m_state->lock);
    m_state->ends[m_side] = nullptr;
    if (Channel* peer = m_state->ends[m_side ^ 1]) {
        peer->signal_clear(SIGNAL_WRITABLE);
        peer->signal_set(SIGNAL_PEER_CLOSED);
    }
}

ktl::result<void> Channel::write(MessageBuffer message) {
    kernel::synchronization::lock_guard guard(m_state->lock);
    Channel* peer = m_state->ends[m_side ^ 1];
    if (!peer) { return ktl::err(ktl::errc::peer_closed); }

    auto& queue = m_state->inbound[m_side ^ 1];
    if (queue.count == QUEUE_DEPTH) { return ktl::err(ktl::errc::capacity_exhausted); }
    queue.slots[(queue.head + queue.count) % QUEUE_DEPTH] = ktl::move(message);
    queue.count++;

    peer->signal_set(SIGNAL_READABLE);
    if (queue.count == QUEUE_DEPTH) { signal_clear(SIGNAL_WRITABLE); }
    return ktl::result<void>::ok();
}

ktl::result<MessageBuffer> Channel::read(size_t max_bytes) {
    kernel::synchronization::lock_guard guard(m_state->lock);
    auto& queue = m_state->inbound[m_side];
    if (queue.count == 0) {
        bool peer_alive = m_state->ends[m_side ^ 1] != nullptr;
        return ktl::err(peer_alive ? ktl::errc::would_block : ktl::errc::peer_closed);
    }

    MessageBuffer& front = queue.slots[queue.head];
    if (front.size() > max_bytes) { return ktl::err(ktl::errc::truncated); }

    bool was_full         = queue.count == QUEUE_DEPTH;
    MessageBuffer message = ktl::move(front);
    queue.head            = (queue.head + 1) % QUEUE_DEPTH;
    queue.count--;

    if (queue.count == 0) { signal_clear(SIGNAL_READABLE); }
    if (was_full) {
        if (Channel* peer = m_state->ends[m_side ^ 1]) { peer->signal_set(SIGNAL_WRITABLE); }
    }
    return ktl::result<MessageBuffer>::ok(ktl::move(message));
}

}  // namespace kernel::obj
