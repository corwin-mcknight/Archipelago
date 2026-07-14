#pragma once

#include <kernel/synchronization/execution_context.h>
#include <kernel/synchronization/lockdep.h>
#include <stdint.h>

namespace kernel::synchronization {

namespace detail {
template <typename Lock> uint32_t identity(const Lock& lock) {
    if constexpr (requires { lock.lockdep_id(); }) { return lock.lockdep_id(); }
    return 0;
}
}  // namespace detail

template <typename Lock> class lock_guard {
   public:
    explicit lock_guard(Lock& lock, const char* file = __builtin_FILE(), uint32_t line = __builtin_LINE())
        : m_lock(lock), m_identity(detail::identity(lock)) {
        m_lock.lock();
        lockdep::acquired(&m_lock, m_identity, file, line);
    }
    ~lock_guard() {
        lockdep::released(&m_lock, m_identity);
        m_lock.unlock();
    }
    lock_guard(const lock_guard&)            = delete;
    lock_guard& operator=(const lock_guard&) = delete;

   private:
    Lock& m_lock;
    uint32_t m_identity;
};

template <typename Lock> class critical_lock_guard {
   public:
    explicit critical_lock_guard(Lock& lock, const char* file = __builtin_FILE(), uint32_t line = __builtin_LINE())
        : m_critical(), m_guard(lock, file, line) {}

   private:
    critical_section m_critical;
    lock_guard<Lock> m_guard;
};

template <typename Lock> class critical_irq_lock_guard {
   public:
    explicit critical_irq_lock_guard(Lock& lock, const char* file = __builtin_FILE(), uint32_t line = __builtin_LINE())
        : m_critical(), m_guard(lock, file, line) {}

   private:
    critical_irq_section m_critical;
    lock_guard<Lock> m_guard;
};

}  // namespace kernel::synchronization
