#pragma once

#include <kernel/config.h>
#include <stdint.h>

namespace kernel::synchronization {

class spinlock {
   public:
    constexpr spinlock() : m_state(unlocked_state) {}
    spinlock(const spinlock&) = delete;
    spinlock& operator=(const spinlock&) = delete;

    void lock() {
        while (true) {
            if (!__atomic_exchange_n(&m_state, locked_state, __ATOMIC_ACQUIRE)) { return; }
            while (__atomic_load_n(&m_state, __ATOMIC_RELAXED)) {}
        }
    }

    bool try_lock() { return !__atomic_exchange_n(&m_state, locked_state, __ATOMIC_ACQUIRE); }

    void unlock() { __atomic_store_n(&m_state, unlocked_state, __ATOMIC_RELEASE); }

    bool is_locked() const { return __atomic_load_n(&m_state, __ATOMIC_RELAXED) == locked_state; }

   private:
    static constexpr uint8_t unlocked_state = 0;
    static constexpr uint8_t locked_state = 1;
    alignas(CONFIG_CPU_CACHE_LINE_SIZE) mutable uint8_t m_state;
};

}  // namespace kernel::synchronization
