#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/arch.h>
#include <kernel/obj/event.h>
#include <kernel/obj/semaphore.h>
#include <kernel/obj/type_registry.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task.h>
#include <kernel/sched/thread.h>
#include <kernel/sched/wait_queue.h>
#include <kernel/time.h>

#include <ktl/ref>

using namespace kernel::sched;

namespace {
volatile int g_flag;
void set_flag_thread(void*) { g_flag = 1; }
void exit_immediately_thread(void*) {}
volatile int g_pings;
void ping_thread(void*) {
    for (int i = 0; i < 5; ++i) {
        g_pings = g_pings + 1;
        kernel::sched::yield();
    }
}
}  // namespace

KTEST(sched_current_is_kshell_thread, "kernel/sched") {
    auto self = current();
    KTEST_REQUIRE_TRUE(static_cast<bool>(self));
    KTEST_EXPECT_TRUE(self->state() == thread_state::RUNNING);
}

KTEST(sched_spawn_runs, "kernel/sched") {
    g_flag = 0;
    KTEST_UNWRAP(t, spawn("flagger", set_flag_thread, nullptr));
    for (int i = 0; i < 100000 && g_flag == 0; ++i) { yield(); }
    KTEST_EXPECT_EQUAL(g_flag, 1);
}

KTEST(sched_yield_interleaves, "kernel/sched") {
    g_pings = 0;
    KTEST_UNWRAP(t, spawn("pinger", ping_thread, nullptr));
    for (int i = 0; i < 100000 && g_pings < 5; ++i) { yield(); }
    KTEST_EXPECT_EQUAL(g_pings, 5);
}

KTEST(sched_exit_reaps_thread, "kernel/sched") {
    using kernel::obj::g_type_registry;
    uint32_t before = g_type_registry.live_count(Thread::TYPE_ID);
    {
        KTEST_UNWRAP(t, spawn("exiter", exit_immediately_thread, nullptr));
        // t drops at scope end; the zombie + reaper path must release the rest.
    }
    for (int i = 0; i < 100000 && g_type_registry.live_count(Thread::TYPE_ID) > before; ++i) { yield(); }
    KTEST_EXPECT_EQUAL(g_type_registry.live_count(Thread::TYPE_ID), before);
}

namespace {
volatile uint64_t g_spin_count;
volatile bool g_spin_stop;
void spinner_thread(void*) {
    while (!g_spin_stop) { g_spin_count = g_spin_count + 1; }
}
}  // namespace

KTEST(sched_preempts_spinner, "kernel/sched") {
    g_spin_count = 0;
    g_spin_stop  = false;
    KTEST_UNWRAP(t, spawn("spinner", spinner_thread, nullptr));
    // Busy-wait WITHOUT yielding: only preemption can give the spinner CPU time.
    ktime_t start = kernel::time::now();
    while (kernel::time::now() < start + 3 * CONFIG_SCHED_TIMESLICE_TICKS) {}
    uint64_t observed = g_spin_count;
    g_spin_stop       = true;
    KTEST_EXPECT_TRUE(observed > 0);
    for (int i = 0; i < 100000 && t->state() != thread_state::DEAD; ++i) { yield(); }
    KTEST_EXPECT_TRUE(t->state() == thread_state::DEAD);
}

namespace {
kernel::sched::wait_queue g_test_wq;
volatile int g_blocked_phase;
void blocker_thread(void*) {
    g_blocked_phase = 1;
    g_test_wq.block();
    g_blocked_phase = 2;
}
}  // namespace

KTEST(sched_sleep_advances_time, "kernel/sched") {
    ktime_t before = kernel::time::now();
    sleep_ticks(5);
    KTEST_EXPECT_TRUE(kernel::time::now() >= before + 5);
}

KTEST(sched_block_and_wake, "kernel/sched") {
    g_blocked_phase = 0;
    KTEST_UNWRAP(t, spawn("blocker", blocker_thread, nullptr));
    for (int i = 0; i < 100000 && g_blocked_phase == 0; ++i) { yield(); }
    KTEST_REQUIRE_EQUAL(g_blocked_phase, 1);
    for (int i = 0; i < 100000; ++i) {
        if (t->state() == thread_state::BLOCKED) break;
        yield();
    }
    KTEST_REQUIRE_TRUE(t->state() == thread_state::BLOCKED);
    sleep_ticks(3);  // give it slices; it must stay blocked, not spin
    KTEST_EXPECT_EQUAL(g_blocked_phase, 1);
    KTEST_EXPECT_TRUE(t->state() == thread_state::BLOCKED);
    g_test_wq.wake_one();
    for (int i = 0; i < 100000 && g_blocked_phase != 2; ++i) { yield(); }
    KTEST_EXPECT_EQUAL(g_blocked_phase, 2);
}

namespace {
void sleep_then_exit_thread(void*) { kernel::sched::sleep_ticks(3); }
}  // namespace

KTEST(sched_join_via_terminated_signal, "kernel/sched") {
    KTEST_UNWRAP(t, spawn("mortal", sleep_then_exit_thread, nullptr));
    uint32_t sig = t->wait_signals(Thread::SIGNAL_TERMINATED);
    KTEST_EXPECT_TRUE((sig & Thread::SIGNAL_TERMINATED) != 0);
    KTEST_EXPECT_TRUE(t->state() == thread_state::DEAD);
}

namespace {
// Bundles a mask==0 waiter and a signal (nonzero-mask) waiter with the shared object they park on,
// so the two directions of the mask-disjointness contract (wake_one/wake_matching only ever cross
// their own lane) can be driven from a single spawned thread each.
struct EventWaitArgs {
    kernel::obj::Event* ev;
    volatile int* phase;
};

constexpr uint32_t TEST_SIGNAL = 1u << 3;

void mask_zero_wait_thread(void* arg) {
    auto* a   = static_cast<EventWaitArgs*>(arg);
    *a->phase = 1;
    a->ev->waiters().block(0);
    *a->phase = 2;
}

void signal_wait_thread(void* arg) {
    auto* a   = static_cast<EventWaitArgs*>(arg);
    *a->phase = 1;
    a->ev->wait_signals(TEST_SIGNAL);
    *a->phase = 2;
}
}  // namespace

// wake_matching (driven here via signal_set) must never wake a mask==0 waiter -- only wake_one/
// wake_all may.
KTEST(sched_mask_zero_ignores_signal_wake, "kernel/sched") {
    kernel::obj::Event ev;
    volatile int phase = 0;
    EventWaitArgs args{&ev, &phase};
    KTEST_UNWRAP(t, spawn("mask0", mask_zero_wait_thread, &args));
    for (int i = 0; i < 100000 && phase == 0; ++i) { yield(); }
    KTEST_REQUIRE_EQUAL(phase, 1);
    for (int i = 0; i < 100000; ++i) {
        if (t->state() == thread_state::BLOCKED) break;
        yield();
    }
    KTEST_REQUIRE_TRUE(t->state() == thread_state::BLOCKED);
    ev.signal_set(TEST_SIGNAL);
    sleep_ticks(3);  // give it slices; a wrongly-woken waiter would have advanced by now
    KTEST_EXPECT_EQUAL(phase, 1);
    KTEST_EXPECT_TRUE(t->state() == thread_state::BLOCKED);
    ev.waiters().wake_one();
    for (int i = 0; i < 100000 && phase != 2; ++i) { yield(); }
    KTEST_EXPECT_EQUAL(phase, 2);
}

// wake_one/wake_all must never wake a signal (nonzero-mask) waiter -- only wake_matching may.
KTEST(sched_signal_waiter_ignores_wake_one, "kernel/sched") {
    kernel::obj::Event ev;
    volatile int phase = 0;
    EventWaitArgs args{&ev, &phase};
    KTEST_UNWRAP(t, spawn("sigwait", signal_wait_thread, &args));
    for (int i = 0; i < 100000 && phase == 0; ++i) { yield(); }
    KTEST_REQUIRE_EQUAL(phase, 1);
    for (int i = 0; i < 100000; ++i) {
        if (t->state() == thread_state::BLOCKED) break;
        yield();
    }
    KTEST_REQUIRE_TRUE(t->state() == thread_state::BLOCKED);
    ev.waiters().wake_one();
    sleep_ticks(3);
    KTEST_EXPECT_EQUAL(phase, 1);
    KTEST_EXPECT_TRUE(t->state() == thread_state::BLOCKED);
    ev.signal_set(TEST_SIGNAL);
    for (int i = 0; i < 100000 && phase != 2; ++i) { yield(); }
    KTEST_EXPECT_EQUAL(phase, 2);
}

namespace {
volatile int g_sem_phase;
void sem_acquirer_thread(void* arg) {
    auto* sem   = static_cast<kernel::obj::Semaphore*>(arg);
    g_sem_phase = 1;
    sem->acquire();
    g_sem_phase = 2;
}
}  // namespace

KTEST(sched_semaphore_blocks_and_wakes, "kernel/sched") {
    auto sem    = ktl::make_ref<kernel::obj::Semaphore>(0u);
    g_sem_phase = 0;
    KTEST_UNWRAP(t, spawn("acquirer", sem_acquirer_thread, sem.get()));
    for (int i = 0; i < 100000 && g_sem_phase == 0; ++i) { yield(); }
    KTEST_REQUIRE_EQUAL(g_sem_phase, 1);
    for (int i = 0; i < 100000; ++i) {
        if (t->state() == thread_state::BLOCKED) break;
        yield();
    }
    KTEST_REQUIRE_TRUE(t->state() == thread_state::BLOCKED);
    sleep_ticks(3);  // must stay blocked across slices
    KTEST_EXPECT_EQUAL(g_sem_phase, 1);
    sem->release();
    for (int i = 0; i < 100000 && g_sem_phase != 2; ++i) { yield(); }
    KTEST_EXPECT_EQUAL(g_sem_phase, 2);
    uint32_t sig = t->wait_signals(kernel::sched::Thread::SIGNAL_TERMINATED);
    KTEST_EXPECT_TRUE((sig & kernel::sched::Thread::SIGNAL_TERMINATED) != 0);
}

namespace {
volatile uint64_t g_recursion_sink;
__attribute__((noinline)) uint64_t recurse_forever(uint64_t depth) {
    volatile uint64_t pad[32];
    pad[0]           = depth;
    g_recursion_sink = pad[0];
    if (depth < (1ull << 40)) { return recurse_forever(depth + 1) + pad[31]; }
    return pad[31];
}
}  // namespace

KTEST_CRASH_TEST(sched_stack_tripwire, "kernel/sched") { (void)recurse_forever(0); }

KTEST(sched_timestamp_monotonic_and_calibrated, "kernel/sched") {
    uint64_t hz = kernel::arch::timestamp_hz();
    KTEST_EXPECT_TRUE(hz > 0);  // riscv constant; x86 calibrated in late_boot
    uint64_t a = kernel::arch::timestamp();
    uint64_t b = kernel::arch::timestamp();
    KTEST_EXPECT_TRUE(b >= a);
    ktime_t t0 = kernel::time::now();
    while (kernel::time::now() < t0 + 2) {}
    KTEST_EXPECT_TRUE(kernel::arch::timestamp() > a);
}

#endif
