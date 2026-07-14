#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/synchronization/spinlock.h>

using namespace kernel::testing;
using kernel::synchronization::critical_section;
using kernel::synchronization::spinlock;

KTEST_MODULE("kernel/spinlock");

// Single-threaded state-machine check over one lock; the contended spin and the
// irq-safe lock_guard are exercised in the QEMU tier where interrupts are real.
KTEST_CASE(spinlock_state_machine) {
    spinlock lock;
    critical_section critical;

    // lock/unlock roundtrip
    KTEST_EXPECT_FALSE(lock.is_locked());
    lock.lock();
    KTEST_EXPECT_TRUE(lock.is_locked());
    lock.unlock();
    KTEST_EXPECT_FALSE(lock.is_locked());

    // try_lock: fails while held, succeeds again after release
    KTEST_REQUIRE_TRUE(lock.try_lock());
    KTEST_EXPECT_FALSE(lock.try_lock());  // already held
    lock.unlock();
    KTEST_EXPECT_TRUE(lock.try_lock());
    lock.unlock();
}

#endif  // CONFIG_KERNEL_TESTING
