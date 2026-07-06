#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/synchronization/spinlock.h>

using namespace kernel::testing;
using kernel::synchronization::spinlock;

// Single-threaded state-machine checks; the contended spin and the irq-safe
// lock_guard are exercised in the QEMU tier where interrupts are real.

KTEST(spinlock_lock_unlock_roundtrip, "kernel/spinlock") {
    spinlock lock;
    KTEST_EXPECT_FALSE(lock.is_locked());
    lock.lock();
    KTEST_EXPECT_TRUE(lock.is_locked());
    lock.unlock();
    KTEST_EXPECT_FALSE(lock.is_locked());
}

KTEST(spinlock_try_lock, "kernel/spinlock") {
    spinlock lock;
    KTEST_REQUIRE_TRUE(lock.try_lock());
    KTEST_EXPECT_FALSE(lock.try_lock());  // already held
    lock.unlock();
    KTEST_EXPECT_TRUE(lock.try_lock());
    lock.unlock();
}

#endif  // CONFIG_KERNEL_TESTING
