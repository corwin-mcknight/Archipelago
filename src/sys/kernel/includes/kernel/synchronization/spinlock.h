#pragma once

#include <kernel/config.h>
#include <kernel/x86/descriptor_tables.h>
#include <stdint.h>

namespace kernel::synchronization {

class spinlock {
   public:
    constexpr spinlock() : m_state(unlocked_state) {}
    spinlock(const spinlock&)            = delete;
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
    static constexpr uint8_t locked_state   = 1;
    alignas(CONFIG_CPU_CACHE_LINE_SIZE) mutable volatile uint8_t m_state;
};

// IRQ-safe RAII guard. Disables interrupts on this CPU before taking the lock and restores the
// prior interrupt state after releasing it, so a held lock can never be reentered by an interrupt
// handler on the same CPU (the F012 self-deadlock). The cli-before-lock / unlock-before-restore
// ordering is what closes the window: interrupts are masked for exactly the duration the lock is
// held. Saving the prior RFLAGS (rather than unconditionally re-enabling) keeps nesting correct.
class lock_guard {
   public:
    explicit lock_guard(spinlock& lock) : m_lock(lock) {
        m_flags = kernel::x86::save_and_disable_interrupts();
        m_lock.lock();
    }
    ~lock_guard() {
        m_lock.unlock();
        kernel::x86::restore_interrupts(m_flags);
    }
    lock_guard(const lock_guard&)            = delete;
    lock_guard& operator=(const lock_guard&) = delete;

   private:
    spinlock& m_lock;
    uint64_t m_flags = 0;
};

}  // namespace kernel::synchronization
