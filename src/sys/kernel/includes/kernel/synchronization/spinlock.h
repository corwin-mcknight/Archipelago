#pragma once

#include <kernel/config.h>
#include <kernel/panic.h>
#include <kernel/synchronization/execution_context.h>
#include <kernel/synchronization/lockdep.h>
#include <stdint.h>

namespace kernel::synchronization {

class spinlock {
   public:
#ifndef NDEBUG
    spinlock() : m_lockdep_id(lockdep::allocate_identity(this, "spinlock")) {}
#else
    spinlock() = default;
#endif
    ~spinlock() {
        lockdep::assert_not_owned(this, m_lockdep_id);
        lockdep::release_identity(m_lockdep_id);
    }
    spinlock(const spinlock&)            = delete;
    spinlock& operator=(const spinlock&) = delete;

    void lock() {
        if (!preemption_disabled()) { panic("spinlock: preemption must be disabled before lock"); }
        while (true) {
            if (!__atomic_exchange_n(&m_state, locked_state, __ATOMIC_ACQUIRE)) { return; }
            while (__atomic_load_n(&m_state, __ATOMIC_RELAXED)) {}
        }
    }
    bool try_lock() {
        if (!preemption_disabled()) { panic("spinlock: preemption must be disabled before try_lock"); }
        return !__atomic_exchange_n(&m_state, locked_state, __ATOMIC_ACQUIRE);
    }
    void unlock() { __atomic_store_n(&m_state, unlocked_state, __ATOMIC_RELEASE); }
    bool is_locked() const { return __atomic_load_n(&m_state, __ATOMIC_RELAXED) == locked_state; }
    uint32_t lockdep_id() const { return m_lockdep_id; }

   private:
    static constexpr uint8_t unlocked_state                      = 0;
    static constexpr uint8_t locked_state                        = 1;
    alignas(CONFIG_CPU_CACHE_LINE_SIZE) volatile uint8_t m_state = unlocked_state;
#ifndef NDEBUG
    uint32_t m_lockdep_id;
#else
    static constexpr uint32_t m_lockdep_id = 0;
#endif
};

}  // namespace kernel::synchronization

#include <kernel/synchronization/guard.h>
