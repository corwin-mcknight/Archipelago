// src/sys/kernel/tests/sched_thread_test.cpp
#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/obj/type_registry.h>
#include <kernel/sched/thread.h>

using namespace kernel::sched;
using kernel::obj::g_type_registry;

static void sched_thread_init() {
    // Fork-per-test isolation gives each test a fresh registry; direct registration is safe.
    Thread::register_type(g_type_registry).expect("thread type registration failed");
}

KTEST_WITH_INIT(sched_thread_defaults, "sched/thread", sched_thread_init) {
    Thread t;
    KTEST_EXPECT_TRUE(t.state() == thread_state::READY);
    KTEST_EXPECT_EQUAL(t.saved_sp(), 0u);
    KTEST_EXPECT_EQUAL(t.kstack_phys(), 0u);
    KTEST_EXPECT_EQUAL(t.kstack_floor(), 0u);
    KTEST_EXPECT_TRUE(t.type_id() == kernel::obj::type_ids::THREAD);
}

KTEST_WITH_INIT(sched_thread_stack_ctor_sets_floor, "sched/thread", sched_thread_init) {
    Thread t(0x40000, 0xFFFF800000040000u);
    KTEST_EXPECT_EQUAL(t.kstack_phys(), 0x40000u);
    KTEST_EXPECT_EQUAL(t.kstack_floor(), 0xFFFF800000040000u + CONFIG_KERNEL_STACK_TRIPWIRE_MARGIN);
}

KTEST_WITH_INIT(sched_thread_state_transitions, "sched/thread", sched_thread_init) {
    Thread t;
    t.set_state(thread_state::RUNNING);
    KTEST_EXPECT_TRUE(t.state() == thread_state::RUNNING);
    t.set_state(thread_state::DEAD);
    KTEST_EXPECT_TRUE(t.state() == thread_state::DEAD);
}

KTEST_WITH_INIT(sched_thread_slice_saturates, "sched/thread", sched_thread_init) {
    Thread t;
    t.reset_slice();
    for (uint32_t i = 0; i < CONFIG_SCHED_TIMESLICE_TICKS - 1; ++i) { KTEST_EXPECT_TRUE(t.decrement_slice() > 0); }
    KTEST_EXPECT_EQUAL(t.decrement_slice(), 0u);
    KTEST_EXPECT_EQUAL(t.decrement_slice(), 0u);  // saturates, no wrap
}

KTEST_WITH_INIT(sched_thread_terminated_signal, "sched/thread", sched_thread_init) {
    Thread t;
    t.signal_set(Thread::SIGNAL_TERMINATED);
    KTEST_EXPECT_TRUE((t.signals() & Thread::SIGNAL_TERMINATED) != 0);
}

#endif
