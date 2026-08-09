#include <kernel/mm/object_arena.h>
#include <kernel/obj/port.h>
#include <kernel/synchronization/guard.h>
#include <kernel/synchronization/spinlock.h>
#include <std/new.h>

namespace kernel::obj {

namespace {

// ponytail: one lock for the whole port subsystem -- every binding list, every port's ready
// queue -- and a plain (non-IRQ) guard, because nothing signals objects from interrupt context
// yet. Both are ceilings: split the lock if contention shows, switch to the IRQ guard when
// interrupt objects start signaling from handlers.
kernel::synchronization::spinlock g_port_lock;

kernel::mm::object_arena g_binding_arena("port-binding", sizeof(port_binding), alignof(port_binding));

void destroy_binding(port_binding* binding) {
    binding->~port_binding();  // drops the object reference
    g_binding_arena.free(binding);
}

}  // namespace

// Unlink from the watched object's doubly-linked list. Caller holds g_port_lock.
void Port::unlink_from_object(port_binding* binding) {
    Object* watched = binding->object.get();
    if (binding->obj_prev != nullptr) { binding->obj_prev->obj_next = binding->obj_next; }
    if (binding->obj_next != nullptr) { binding->obj_next->obj_prev = binding->obj_prev; }
    if (watched->m_bindings == binding) { watched->m_bindings = binding->obj_next; }
    binding->obj_next = nullptr;
    binding->obj_prev = nullptr;
}

// Caller holds g_port_lock.
void Port::enqueue_ready(port_binding* binding) {
    binding->ready_next = nullptr;
    if (m_ready_tail != nullptr) {
        m_ready_tail->ready_next = binding;
    } else {
        m_ready_head = binding;
    }
    m_ready_tail = binding;
}

ktl::result<void> Port::bind(ktl::ref<Object> object, uint64_t key, uint32_t mask) {
    if (!object) { return ktl::err(ktl::errc::null_argument); }
    if (mask == 0) { return ktl::err(ktl::errc::invalid_operation); }

    void* slot = g_binding_arena.alloc();
    if (slot == nullptr) { return ktl::err(ktl::errc::oom); }
    auto* binding   = new (slot) port_binding{};
    binding->key    = key;
    binding->mask   = mask;
    binding->object = ktl::move(object);
    binding->port   = this;

    bool pended     = false;
    {
        kernel::synchronization::critical_lock_guard guard(g_port_lock);
        Object* watched   = binding->object.get();
        binding->obj_next = watched->m_bindings;
        if (watched->m_bindings != nullptr) { watched->m_bindings->obj_prev = binding; }
        watched->m_bindings = binding;
        binding->port_next  = m_bindings;
        m_bindings          = binding;

        // A signal already inside the mask queues immediately: binding a readable channel must
        // not wait for a fresh transition that may never come.
        uint32_t asserted   = watched->signals() & mask;
        if (asserted != 0) {
            binding->pending         = true;
            binding->pending_signals = asserted;
            enqueue_ready(binding);
            pended = true;
        }
    }
    if (pended) { signal_set(SIGNAL_READABLE); }
    return ktl::result<void>::ok();
}

size_t Port::unbind(uint64_t key) {
    // Unlink matches under the lock, destroy them after: the dropped object reference may run an
    // arbitrary destructor, which must not execute inside the port lock.
    port_binding* victims = nullptr;
    {
        kernel::synchronization::critical_lock_guard guard(g_port_lock);
        port_binding** link = &m_bindings;
        while (*link != nullptr) {
            port_binding* binding = *link;
            if (binding->key != key) {
                link = &binding->port_next;
                continue;
            }
            *link = binding->port_next;
            unlink_from_object(binding);
            if (binding->pending) {
                // Drop the pending packet: walk the singly-linked ready FIFO to excise it.
                port_binding** ready = &m_ready_head;
                while (*ready != binding) { ready = &(*ready)->ready_next; }
                *ready = binding->ready_next;
                if (m_ready_tail == binding) {
                    m_ready_tail = m_ready_head;
                    while (m_ready_tail != nullptr && m_ready_tail->ready_next != nullptr) {
                        m_ready_tail = m_ready_tail->ready_next;
                    }
                }
            }
            binding->port_next = victims;
            victims            = binding;
        }
        if (m_ready_head == nullptr) { signal_clear(SIGNAL_READABLE); }
    }

    size_t count = 0;
    while (victims != nullptr) {
        port_binding* next = victims->port_next;
        destroy_binding(victims);
        victims = next;
        count++;
    }
    return count;
}

Port::~Port() {
    // No new bindings can arrive (no handles remain) and every binding holds only objects, never
    // the port, so tearing the lists down here cannot race a bind. Signal delivery can still find
    // bindings until they leave the object lists under the lock.
    port_binding* victims = nullptr;
    {
        kernel::synchronization::critical_lock_guard guard(g_port_lock);
        while (m_bindings != nullptr) {
            port_binding* binding = m_bindings;
            m_bindings            = binding->port_next;
            unlink_from_object(binding);
            binding->port_next = victims;
            victims            = binding;
        }
        m_ready_head = nullptr;
        m_ready_tail = nullptr;
    }
    while (victims != nullptr) {
        port_binding* next = victims->port_next;
        destroy_binding(victims);
        victims = next;
    }
}

bool Port::dequeue(Packet& out) {
    kernel::synchronization::critical_lock_guard guard(g_port_lock);
    port_binding* binding = m_ready_head;
    if (binding == nullptr) {
        // Cleared under the lock so a producer's enqueue cannot interleave; the producer's
        // signal_set lands after its unlock, so READABLE can flicker high on an empty queue but
        // never sit low on a full one.
        signal_clear(SIGNAL_READABLE);
        return false;
    }
    m_ready_head = binding->ready_next;
    if (m_ready_head == nullptr) { m_ready_tail = nullptr; }
    binding->ready_next      = nullptr;
    out.key                  = binding->key;
    out.signals              = binding->pending_signals;
    binding->pending         = false;
    binding->pending_signals = 0;
    if (m_ready_head == nullptr) { signal_clear(SIGNAL_READABLE); }
    return true;
}

size_t Port::binding_count() {
    kernel::synchronization::critical_lock_guard guard(g_port_lock);
    size_t count = 0;
    for (port_binding* b = m_bindings; b != nullptr; b = b->port_next) { count++; }
    return count;
}

void port_notify(Object* object, uint32_t previous, uint32_t current) {
    // The restart dance keeps the port lock out of signal_set's way: asserting READABLE on the
    // port re-enters this function for the port's own bindings, so it must happen unlocked. Each
    // restart finds strictly more bindings already pending, so the scan converges.
    for (;;) {
        Port* to_signal = nullptr;
        {
            kernel::synchronization::critical_lock_guard guard(g_port_lock);
            for (port_binding* binding = object->m_bindings; binding != nullptr; binding = binding->obj_next) {
                uint32_t hits = current & binding->mask;
                if (hits == 0) { continue; }
                if (binding->pending) {
                    binding->pending_signals |= hits;  // coalesce into the queued packet
                    continue;
                }
                if ((previous & binding->mask) != 0) { continue; }  // no edge: was already matched
                binding->pending         = true;
                binding->pending_signals = hits;
                binding->port->enqueue_ready(binding);
                to_signal = binding->port;
                break;
            }
        }
        if (to_signal == nullptr) { return; }
        to_signal->signal_set(Port::SIGNAL_READABLE);
    }
}

}  // namespace kernel::obj
