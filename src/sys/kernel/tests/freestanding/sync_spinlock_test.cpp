#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/synchronization/spinlock.h>
#include <kernel/x86/descriptor_tables.h>

using namespace kernel::testing;
using kernel::synchronization::lock_guard;
using kernel::synchronization::spinlock;

// lock_guard acquires/releases the underlying lock.
KTEST(spinlock_lock_guard_acquires, "kernel/spinlock") {
    spinlock lock;
    KTEST_REQUIRE_TRUE(!lock.is_locked());
    {
        lock_guard guard(lock);
        KTEST_EXPECT_TRUE(lock.is_locked());
    }
    KTEST_EXPECT_TRUE(!lock.is_locked());
}

// F012: lock_guard disables interrupts while the lock is held and restores the prior state after.
KTEST(spinlock_lock_guard_is_irqsave, "kernel/spinlock") {
    spinlock lock;
    kernel::x86::enable_interrupts();  // establish IF=1 going in (the normal post-boot state)
    KTEST_REQUIRE_TRUE(kernel::x86::interrupts_enabled());
    {
        lock_guard guard(lock);
        KTEST_EXPECT_TRUE(!kernel::x86::interrupts_enabled());  // masked while held
    }
    KTEST_EXPECT_TRUE(kernel::x86::interrupts_enabled());  // restored to prior (enabled) state
}

// Nested guards keep interrupts masked until the OUTERMOST guard releases, then restore.
KTEST(spinlock_lock_guard_irqsave_nests, "kernel/spinlock") {
    spinlock a;
    spinlock b;
    kernel::x86::enable_interrupts();
    {
        lock_guard ga(a);
        KTEST_EXPECT_TRUE(!kernel::x86::interrupts_enabled());
        {
            lock_guard gb(b);
            KTEST_EXPECT_TRUE(!kernel::x86::interrupts_enabled());
        }
        KTEST_EXPECT_TRUE(!kernel::x86::interrupts_enabled());  // inner restore keeps IF masked (outer holds)
    }
    KTEST_EXPECT_TRUE(kernel::x86::interrupts_enabled());  // outer restore re-enables
}

// save_and_disable / restore round-trips the interrupt-enable flag.
KTEST(interrupts_save_restore_roundtrip, "kernel/spinlock") {
    kernel::x86::enable_interrupts();
    uint64_t flags = kernel::x86::save_and_disable_interrupts();
    KTEST_EXPECT_TRUE(!kernel::x86::interrupts_enabled());  // disabled by the save
    kernel::x86::restore_interrupts(flags);
    KTEST_EXPECT_TRUE(kernel::x86::interrupts_enabled());  // restored to the saved (enabled) state
}

#endif  // CONFIG_KERNEL_TESTING
