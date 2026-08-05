#pragma once

#include <stdint.h>

#include <ktl/atomic>

#include "kernel/config.h"

namespace kernel::synchronization {

/**
 * A semaphore utilizing an atomic counter to manage access to a resource.
 * This implementation utilizes a spin-wait mechanism to block threads when the
 * semaphore count is zero.
 * @author Corwin McKnight
 */
class semaphore {
   public:
    explicit constexpr semaphore(uint32_t initial_count = 1) : m_count(initial_count) {}
    semaphore(const semaphore&)            = delete;
    semaphore& operator=(const semaphore&) = delete;

    /// Acquire the semaphore, blocking if necessary.
    void acquire() {
        while (true) {
            uint32_t current = m_count.load(ktl::memory_order::relaxed);
            while (current == 0) { current = m_count.load(ktl::memory_order::relaxed); }
            if (m_count.compare_exchange(current, current - 1, ktl::memory_order::acquire,
                                         ktl::memory_order::relaxed)) {
                return;
            }
        }
    }

    /// Try to acquire the semaphore without blocking.
    /// @return true if the semaphore was acquired, false otherwise.
    bool try_acquire() {
        uint32_t current = m_count.load(ktl::memory_order::relaxed);
        while (current != 0) {
            if (m_count.compare_exchange(current, current - 1, ktl::memory_order::acquire,
                                         ktl::memory_order::relaxed)) {
                return true;
            }
        }
        return false;
    }

    void release() { m_count.fetch_add(1, ktl::memory_order::release); }

    uint32_t count() const { return m_count.load(ktl::memory_order::relaxed); }

   private:
    alignas(CONFIG_CPU_CACHE_LINE_SIZE) ktl::atomic<uint32_t> m_count;
};

}  // namespace kernel::synchronization
