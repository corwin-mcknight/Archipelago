#pragma once

#include <kernel/obj/object.h>
#include <kernel/obj/type_registry.h>
#include <kernel/obj/types.h>

#include <ktl/result>

namespace kernel::obj {

// Counting semaphore as a kernel object: acquire() blocks the calling thread on the object's
// waiter queue. Pre-scheduler code uses the spin-wait kernel::synchronization::semaphore instead.
class Semaphore : public Object {
   public:
    DECLARE_OBJECT_TYPE(Semaphore, type_ids::SEMAPHORE)

    explicit Semaphore(uint32_t initial_count = 1) : Object(TYPE_ID), m_count(initial_count) {}

    /// Blocks until a unit is available. Kernel-only (defined in core/sched/wait_queue.cpp).
    void acquire();
    /// Releases one unit and wakes one blocked acquirer. Kernel-only.
    void release();

    bool try_acquire() {
        uint32_t current = __atomic_load_n(&m_count, __ATOMIC_RELAXED);
        while (current != 0) {
            if (__atomic_compare_exchange_n(&m_count, &current, current - 1, false, __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED)) {
                return true;
            }
        }
        return false;
    }

    uint32_t count() const { return __atomic_load_n(&m_count, __ATOMIC_RELAXED); }

    static ktl::result<void> register_type(TypeRegistry& registry) {
        return registry.register_type(TYPE_ID, "semaphore", RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE,
                                      RIGHT_READ | RIGHT_WRITE);
    }

   private:
    volatile uint32_t m_count;
};

}  // namespace kernel::obj
