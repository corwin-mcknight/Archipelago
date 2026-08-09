#pragma once

#include <abi/syscall.h>
#include <kernel/obj/object.h>
#include <kernel/obj/type_registry.h>
#include <kernel/obj/types.h>

#include <ktl/ref>
#include <ktl/result>

namespace kernel::obj {

class Port;

// One signal subscription: some object's signal transitions, filtered by mask, delivered on a
// port as a packet carrying the binder's key (docs/Design/IPC Primitives.md). The binding IS the
// packet storage -- allocated once at bind time from its arena, so signal delivery allocates
// nothing and repeated transitions coalesce into the pending state instead of growing a queue.
// The object reference is strong: a bound object stays alive until unbound, which is what lets
// the binding live on the object's list with no destructor hook in Object.
struct port_binding {
    uint64_t key             = 0;
    uint32_t mask            = 0;
    bool pending             = false;  // queued on the port's ready list
    uint32_t pending_signals = 0;
    ktl::ref<Object> object;
    Port* port               = nullptr;
    port_binding* obj_next   = nullptr;  // the watched object's binding list (doubly linked)
    port_binding* obj_prev   = nullptr;
    port_binding* port_next  = nullptr;  // the port's list of all its bindings
    port_binding* ready_next = nullptr;  // the port's FIFO of pending packets
};

// A multi-producer, single-consumer packet queue over signal bindings: the server-side
// multiplexing point (docs/Design/IPC Primitives.md). Binding an object's signals to a port turns
// transitions into packets; the port asserts READABLE while packets are pending, so waiting on
// many objects is one wait on the port. Delivery is edge-triggered -- a packet queues when the
// signal state transitions into the mask (or is already in it at bind time), and re-arms when the
// packet is dequeued -- with repeats coalescing into the still-pending packet.
class Port : public Object {
   public:
    DECLARE_OBJECT_TYPE(Port, type_ids::PORT)

    static constexpr uint32_t SIGNAL_READABLE = static_cast<uint32_t>(::abi::syscall::PORT_SIGNAL_READABLE);

    // Single consumer by design: DUPLICATE stays outside the valid mask like a channel endpoint.
    static constexpr Rights DEFAULT_RIGHTS    = RIGHT_READ | RIGHT_WRITE | RIGHT_WAIT;

    struct Packet {
        uint64_t key     = 0;
        uint32_t signals = 0;
    };

    Port() : Object(TYPE_ID) {}
    ~Port() override;

    // Subscribe `object`'s signals under `mask`, delivering `key` with each packet. Takes the
    // reference: the object stays alive while bound. A signal already asserted at bind queues a
    // packet immediately, so binding a readable channel cannot miss its existing messages.
    ktl::result<void> bind(ktl::ref<Object> object, uint64_t key, uint32_t mask);

    // Drop every binding with `key`, pending packets included; returns how many were dropped.
    size_t unbind(uint64_t key);

    // Pop the oldest pending packet. false when none are pending; READABLE tracks the queue
    // either way.
    bool dequeue(Packet& out);

    size_t binding_count();

    static ktl::result<void> register_type(TypeRegistry& registry) {
        return registry.register_type(TYPE_ID, "port", DEFAULT_RIGHTS, DEFAULT_RIGHTS);
    }

   private:
    friend void port_notify(Object* object, uint32_t previous, uint32_t current);

    // Both called with the port subsystem lock held.
    void enqueue_ready(port_binding* binding);
    static void unlink_from_object(port_binding* binding);

    port_binding* m_bindings   = nullptr;
    port_binding* m_ready_head = nullptr;
    port_binding* m_ready_tail = nullptr;
};

}  // namespace kernel::obj
