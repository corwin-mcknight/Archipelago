#pragma once

#include <kernel/obj/types.h>
#include <kernel/sched/wait_queue.h>
#include <kernel/time.h>

#include <ktl/atomic>

#define DECLARE_OBJECT_TYPE(ClassName, TypeIdValue) static constexpr kernel::obj::TypeId TYPE_ID = TypeIdValue;

namespace kernel::obj {

class Object {
   public:
    explicit Object(TypeId type_id);
    virtual ~Object();

    ObjectId id() const { return m_id; }
    TypeId type_id() const { return m_type_id; }

    const char* name() const { return m_name; }
    void set_name(const char* name) { m_name = name; }

    uint32_t signals() const;
    void signal_set(uint32_t bits);
    void signal_clear(uint32_t bits);

    /// Waiters parked on this object's signals.
    kernel::sched::wait_queue& waiters() { return m_waiters; }

    /// Block the calling thread until any signal bit in mask is set; returns the signals
    /// observed. mask must be nonzero. Kernel-only (defined in task/wait_queue.cpp).
    uint32_t wait_signals(uint32_t mask);
    // Same, but gives up at `deadline` (a tick count). Returns the signals observed at return:
    // a value not intersecting the mask means the deadline passed first.
    uint32_t wait_signals_deadline(uint32_t mask, ktime_t deadline);

   private:
    friend class Port;
    friend void port_notify(Object* object, uint32_t previous, uint32_t current);

    ObjectId m_id;
    TypeId m_type_id;
    const char* m_name = nullptr;
    ktl::atomic<uint32_t> m_signals{0};
    kernel::sched::wait_queue m_waiters;
    // Port bindings watching this object's signals (obj/port.cpp), guarded by the port
    // subsystem's lock. Each binding holds a strong reference to this object, so a non-empty
    // list means the object cannot be mid-destruction.
    struct port_binding* m_bindings = nullptr;

    static ObjectId allocate_id();
};

void obj_init();

/// Wake waiters whose mask matches the object's current signals. Implemented by the scheduler
/// layer in kernel builds and stubbed by the host runner (hosted tests see signal bits only).
void object_signal_wake(Object* obj);

/// Deliver a signal transition to the port bindings watching `object` (obj/port.cpp). Called by
/// signal_set only when the binding list is (racily) non-empty; bind's asserted-at-bind check
/// covers the transition a racing bind could miss.
void port_notify(Object* object, uint32_t previous, uint32_t current);

}  // namespace kernel::obj
