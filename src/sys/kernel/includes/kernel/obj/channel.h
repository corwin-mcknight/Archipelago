#pragma once

#include <abi/syscall.h>
#include <kernel/obj/handle_table.h>
#include <kernel/obj/object.h>
#include <kernel/obj/type_registry.h>
#include <kernel/obj/types.h>

#include <ktl/ref>
#include <ktl/result>
#include <ktl/utility>

namespace kernel::obj {

struct channel_state;
class Channel;

// Message pages: Allocates from PMM, a message is at most MAX_MESSAGE_BYTES (1 page) via physmap.
// Returns 0 when no page is available.
uintptr_t channel_page_alloc();
void channel_page_free(uintptr_t page);

// A message. A single physical page containing up to MAX_MESSAGE_BYTES.
// Move only (Page returns to PMM on destruction). A 0 length message does not contain a page.
// Can carry handles in transit -- each slot names an entry in the kernel's handle table.
// A destroyed message can close handles in the kernel table (channel dead, failed send).
class MessageBuffer {
   public:
    static constexpr size_t MAX_HANDLES = static_cast<size_t>(::abi::syscall::CHANNEL_MAX_MESSAGE_HANDLES);

    MessageBuffer()                     = default;
    ~MessageBuffer() { reset(); }

    MessageBuffer(MessageBuffer&& other) noexcept : m_page(other.m_page), m_length(other.m_length) {
        other.m_page   = 0;
        other.m_length = 0;
        adopt_handles(other);
    }
    MessageBuffer& operator=(MessageBuffer&& other) noexcept {
        if (this != &other) {
            reset();
            m_page         = other.m_page;
            m_length       = other.m_length;
            other.m_page   = 0;
            other.m_length = 0;
            adopt_handles(other);
        }
        return *this;
    }
    MessageBuffer(const MessageBuffer&)            = delete;
    MessageBuffer& operator=(const MessageBuffer&) = delete;

    // errc::out_of_range past MAX_MESSAGE_BYTES, errc::oom when the PMM has no page.
    static ktl::result<MessageBuffer> create(size_t length);

    uint8_t* data() { return reinterpret_cast<uint8_t*>(m_page); }
    const uint8_t* data() const { return reinterpret_cast<const uint8_t*>(m_page); }
    size_t size() const { return m_length; }

    // Attach an escrowed kernel-table handle; false once every slot is occupied.
    bool attach_handle(HandleId id) {
        if (m_handle_count == MAX_HANDLES) { return false; }
        m_handles[m_handle_count++] = id;
        return true;
    }
    size_t handle_count() const { return m_handle_count; }
    // Hand every escrowed handle to the caller, who becomes responsible for moving each out of
    // the kernel table (or closing it there). Returns the count copied into `out`.
    size_t detach_handles(HandleId (&out)[MAX_HANDLES]) {
        size_t count = m_handle_count;
        for (size_t i = 0; i < count; i++) { out[i] = m_handles[i]; }
        m_handle_count = 0;
        return count;
    }

   private:
    friend class Channel;

    bool carries_endpoint_from_pair(const Channel& channel) const;

    void adopt_handles(MessageBuffer& other) {
        m_handle_count = other.m_handle_count;
        for (size_t i = 0; i < m_handle_count; i++) { m_handles[i] = other.m_handles[i]; }
        other.m_handle_count = 0;
    }

    void reset() {
        if (m_page != 0) { channel_page_free(m_page); }
        m_page   = 0;
        m_length = 0;
        release_handles();  // out of line: closing escrow needs the kernel task
    }
    void release_handles();

    uintptr_t m_page = 0;
    size_t m_length  = 0;
    HandleId m_handles[MAX_HANDLES];
    size_t m_handle_count = 0;
};

// One endpoint of a bidirectional, point-to-point message channel (docs/Design/IPC Primitives.md).
// create() yields the two endpoints as a pair; each is its own kernel object with its own signal
// state, and the queues between them live in state shared by the pair. Messages are opaque byte
// payloads delivered FIFO per direction. Every operation fails immediately rather than blocking --
// a caller that wants to wait parks on the signal bits instead.

// Endpoint of a bidirectional, point-to-point message channel.
class Channel : public Object {
   public:
    DECLARE_OBJECT_TYPE(Channel, type_ids::CHANNEL)

    static constexpr size_t MAX_MESSAGE_BYTES    = 4096;
    static constexpr size_t QUEUE_DEPTH          = 8;

    // Kernel managed signals. READABLE if incoming is non-empty, WRITABLE if incoming is not full, PEER_CLOSED if other
    // end is destroyed.
    static constexpr uint32_t SIGNAL_READABLE    = static_cast<uint32_t>(::abi::syscall::CHANNEL_SIGNAL_READABLE);
    static constexpr uint32_t SIGNAL_WRITABLE    = static_cast<uint32_t>(::abi::syscall::CHANNEL_SIGNAL_WRITABLE);
    static constexpr uint32_t SIGNAL_PEER_CLOSED = static_cast<uint32_t>(::abi::syscall::CHANNEL_SIGNAL_PEER_CLOSED);

    // TRANSFER is the per-channel gate on carrying handles in messages, per the design doc; the
    // handle being sent needs no right of its own until object types start registering TRANSFER.
    static constexpr Rights DEFAULT_RIGHTS       = RIGHT_READ | RIGHT_WRITE | RIGHT_WAIT | RIGHT_TRANSFER;

    struct Pair {
        ktl::ref<Channel> first;
        ktl::ref<Channel> second;
    };
    static ktl::result<Pair> create();

    ~Channel() override;

    // Queue `message` on the peer's incoming queue. peer_closed if the opposite endpoint is gone,
    // capacity_exhausted if the peer's queue is full (wait on WRITABLE and retry). The size cap
    // is enforced where the message is created, so an in-hand MessageBuffer always fits.
    ktl::result<void> write(MessageBuffer message);

    // Dequeue this endpoint's front message. truncated if it exceeds max_bytes or carries more
    // handles than max_handles -- the message is consumed and discarded (its handles closed), so
    // the next read sees the next message. would_block if the queue is empty, peer_closed if it
    // is empty and can never refill.
    ktl::result<MessageBuffer> read(size_t max_bytes, size_t max_handles = MessageBuffer::MAX_HANDLES);

    // DUPLICATE is deliberately outside the valid mask: an endpoint handle is move-only. Two
    // handles to one endpoint would interleave competing reads and make PEER_CLOSED (which fires
    // on the last handle's close) unreadable as a hangup indicator.
    static ktl::result<void> register_type(TypeRegistry& registry) {
        return registry.register_type(TYPE_ID, "channel", DEFAULT_RIGHTS, DEFAULT_RIGHTS);
    }

    // Use create(); public only because make_ref needs it.
    Channel(ktl::ref<channel_state> state, uint32_t side);

   private:
    friend class MessageBuffer;

    ktl::ref<channel_state> m_state;
    uint32_t m_side;
};

}  // namespace kernel::obj
