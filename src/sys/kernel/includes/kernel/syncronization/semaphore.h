#pragma once

#include <stdint.h>

#include "kernel/config.h"

namespace kernel::synchronization {

class Semaphore {
   public:
    explicit constexpr Semaphore(uint32_t initial_count = 1) : m_count(initial_count) {}
    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    void acquire() {
        while (true) {
            uint32_t current = __atomic_load_n(&m_count, __ATOMIC_RELAXED);
            while (current == 0) { current = __atomic_load_n(&m_count, __ATOMIC_RELAXED); }
            uint32_t desired = current - 1;
            if (__atomic_compare_exchange_n(&m_count, &current, desired, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                return;
            }
        }
    }

    bool try_acquire() {
        uint32_t current = __atomic_load_n(&m_count, __ATOMIC_RELAXED);
        while (current != 0) {
            uint32_t desired = current - 1;
            if (__atomic_compare_exchange_n(&m_count, &current, desired, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                return true;
            }
        }
        return false;
    }

    void release() { __atomic_fetch_add(&m_count, 1, __ATOMIC_RELEASE); }

    uint32_t count() const { return __atomic_load_n(&m_count, __ATOMIC_RELAXED); }

   private:
    alignas(CONFIG_CPU_CACHE_LINE_SIZE) mutable volatile uint32_t m_count;
};

}  // namespace kernel::synchronization
