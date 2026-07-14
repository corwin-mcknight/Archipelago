#pragma once

#include <kernel/sched/wait_queue.h>
#include <kernel/synchronization/execution_context.h>
#include <kernel/synchronization/guard.h>
#include <kernel/synchronization/lockdep.h>
#include <stdint.h>

namespace kernel::synchronization {

class mutex {
   public:
#ifndef NDEBUG
    mutex() : m_lockdep_id(lockdep::allocate_identity(this, "mutex")) {}
#else
    mutex() = default;
#endif
    ~mutex();
    mutex(const mutex&)            = delete;
    mutex& operator=(const mutex&) = delete;

    void lock();
    bool try_lock();
    void unlock();
    bool is_locked() const { return __atomic_load_n(&m_state, __ATOMIC_RELAXED) != 0; }
    uint32_t lockdep_id() const { return m_lockdep_id; }

   private:
    bool try_acquire();

    volatile uint8_t m_state = 0;
    kernel::sched::wait_queue m_waiters;
#ifndef NDEBUG
    uint32_t m_lockdep_id;
#else
    static constexpr uint32_t m_lockdep_id = 0;
#endif
};

}  // namespace kernel::synchronization
