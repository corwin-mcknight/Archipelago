#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/arch.h>
#include <kernel/synchronization/spinlock.h>

using namespace kernel::testing;
using kernel::synchronization::critical_irq_lock_guard;
using kernel::synchronization::critical_lock_guard;
using kernel::synchronization::spinlock;

KTEST_MODULE("kernel/spinlock");

// A critical guard acquires/releases the underlying lock without masking IRQs.
KTEST_CASE(spinlock_lock_guard_acquires) {
    spinlock lock;
    KTEST_REQUIRE_TRUE(!lock.is_locked());
    {
        critical_lock_guard guard(lock);
        KTEST_EXPECT_TRUE(lock.is_locked());
    }
    KTEST_EXPECT_TRUE(!lock.is_locked());
}

// IRQ-critical guards mask interrupts while the lock is held and restore the prior state after,
// including across nesting: interrupts stay masked until the OUTERMOST guard releases.
KTEST_CASE(spinlock_irq_lock_guard_masks_and_nests) {
    spinlock a;
    spinlock b;
    kernel::arch::enable_interrupts();  // establish IF=1 going in (the normal post-boot state)
    KTEST_REQUIRE_TRUE(kernel::arch::interrupts_enabled());
    {
        critical_irq_lock_guard guard(a);
        KTEST_EXPECT_TRUE(!kernel::arch::interrupts_enabled());  // masked while held
    }
    KTEST_EXPECT_TRUE(kernel::arch::interrupts_enabled());  // restored to prior (enabled) state
    {
        critical_irq_lock_guard ga(a);
        KTEST_EXPECT_TRUE(!kernel::arch::interrupts_enabled());
        {
            critical_irq_lock_guard gb(b);
            KTEST_EXPECT_TRUE(!kernel::arch::interrupts_enabled());
        }
        KTEST_EXPECT_TRUE(!kernel::arch::interrupts_enabled());  // inner restore keeps IF masked (outer holds)
    }
    KTEST_EXPECT_TRUE(kernel::arch::interrupts_enabled());  // outer restore re-enables
}

// save_and_disable / restore round-trips the interrupt-enable flag.
KTEST_CASE(interrupts_save_restore_roundtrip) {
    kernel::arch::enable_interrupts();
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    KTEST_EXPECT_TRUE(!kernel::arch::interrupts_enabled());  // disabled by the save
    kernel::arch::restore_interrupts(flags);
    KTEST_EXPECT_TRUE(kernel::arch::interrupts_enabled());  // restored to the saved (enabled) state
}

#endif  // CONFIG_KERNEL_TESTING
