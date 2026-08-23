#include <kernel/panic.h>
#include <kernel/sched/scheduler.h>
#include <kernel/synchronization/mutex.h>

namespace kernel::synchronization {

mutex::~mutex() {
    if (is_locked()) { panic("mutex: destroyed while locked"); }
#if defined(ARCH_X86_64) || defined(ARCH_RISCV64)
    if (m_waiters.has_waiters()) { panic("mutex: destroyed with waiters"); }
#endif
    lockdep::assert_not_owned(this, m_lockdep_id);
    lockdep::release_identity(m_lockdep_id);
}

bool mutex::try_acquire() {
    uint8_t expected = 0;
    return m_state.compare_exchange(expected, 1, ktl::memory_order::acquire, ktl::memory_order::relaxed);
}

bool mutex::try_lock() {
    assert_blocking_allowed("mutex::try_lock: mutex is only valid in preemptible thread context");
    return try_acquire();
}

void mutex::lock() {
    assert_blocking_allowed("mutex::lock: mutex is only valid in preemptible thread context");
    if (try_acquire()) { return; }
#if !defined(ARCH_X86_64) && !defined(ARCH_RISCV64)
    panic("mutex: hosted contention cannot block");
#else
    if (!kernel::sched::started()) { panic("mutex: contention before scheduler startup"); }
    while (true) {
        struct acquire_context {
            mutex* lock;
            bool acquired;
        } context{this, false};
        m_waiters.block_if(
            0,
            [](void* argument) {
                auto* acquire     = static_cast<acquire_context*>(argument);
                acquire->acquired = acquire->lock->try_acquire();
                return !acquire->acquired;
            },
            &context);
        if (context.acquired) { return; }
    }
#endif
}

void mutex::unlock() {
    if (m_state.exchange(0, ktl::memory_order::release) == 0) { panic("mutex: unlock of unlocked mutex"); }
#if defined(ARCH_X86_64) || defined(ARCH_RISCV64)
    m_waiters.wake_one();
#endif
}

}  // namespace kernel::synchronization
