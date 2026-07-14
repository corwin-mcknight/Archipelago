#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/synchronization/execution_context.h>
#include <kernel/synchronization/guard.h>
#include <kernel/synchronization/lockdep.h>
#include <kernel/synchronization/mutex.h>

using namespace kernel::synchronization;

KTEST(sync_context_nesting_restores_depth, "sync/context") {
    init_execution_context(0);
    KTEST_EXPECT_FALSE(preemption_disabled());
    {
        critical_section outer;
        KTEST_EXPECT_EQUAL(current_execution_context().preempt_depth, 1u);
        {
            critical_section inner;
            KTEST_EXPECT_EQUAL(current_execution_context().preempt_depth, 2u);
        }
        KTEST_EXPECT_EQUAL(current_execution_context().preempt_depth, 1u);
    }
    KTEST_EXPECT_FALSE(preemption_disabled());
}

KTEST(sync_mutex_uncontended_and_try_lock, "sync/mutex") {
    init_execution_context(0);
    mutex lock;
    KTEST_REQUIRE_TRUE(lock.try_lock());
    KTEST_EXPECT_FALSE(lock.try_lock());
    lock.unlock();
    {
        lock_guard guard(lock);
        KTEST_EXPECT_TRUE(lock.is_locked());
    }
    KTEST_EXPECT_FALSE(lock.is_locked());
}

KTEST(sync_lockdep_learns_order, "sync/lockdep") {
    init_execution_context(0);
    lockdep::reset_for_testing();
    mutex first;
    mutex second;
    {
        lock_guard a(first);
        lock_guard b(second);
        KTEST_EXPECT_EQUAL(current_execution_context().held_count, 2u);
    }
    KTEST_EXPECT_EQUAL(lockdep::edge_count_for_testing(), 1u);
    KTEST_EXPECT_EQUAL(current_execution_context().held_count, 0u);
}

KTEST(sync_lockdep_reclaims_identities, "sync/lockdep") {
    init_execution_context(0);
    lockdep::reset_for_testing();
    // Each mutex (plus its embedded wait-queue spinlock) must return its identity slot on
    // destruction. Churning far past the fixed pool would panic ("capacity exhausted") if slots
    // leaked, as they did before reclamation existed.
    for (size_t i = 0; i < CONFIG_LOCKDEP_MAX_LOCKS * 3; ++i) {
        mutex m;
        lock_guard guard(m);
    }
    KTEST_EXPECT_EQUAL(current_execution_context().held_count, 0u);
}

#endif  // CONFIG_KERNEL_TESTING
