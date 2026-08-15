#include <kernel/arch.h>
#include <kernel/obj/event.h>
#include <kernel/obj/type_registry.h>
#include <kernel/platform.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task.h>
#include <kernel/sched/thread.h>
#include <kernel/sched/wait_queue.h>
#include <kernel/synchronization/execution_context.h>
#include <kernel/synchronization/mutex.h>
#include <kernel/testing/spawn.h>
#include <kernel/testing/testing.h>
#include <kernel/time.h>

#include <ktl/ref>
#include <ktl/string_view>
#include <ktl/vector>

using namespace kernel::sched;
using kernel::testing::spawn_fn;

KTEST_MODULE("kernel/sched");

KTEST_CASE(sched_current_is_kshell_thread) {
    auto self = current();
    KTEST_REQUIRE_TRUE(static_cast<bool>(self));
    KTEST_EXPECT_TRUE(self->state() == thread_state::RUNNING);
}

KTEST_CASE(sched_spawn_runs) {
    volatile int flag = 0;
    auto body         = [&] { flag = 1; };
    KTEST_UNWRAP(t, spawn_fn("flagger", body));
    KTEST_YIELD_UNTIL(flag == 1);
}

KTEST_CASE(sched_yield_interleaves) {
    volatile int pings = 0;
    auto body          = [&] {
        for (int i = 0; i < 5; ++i) {
            pings = pings + 1;
            kernel::sched::yield();
        }
    };
    KTEST_UNWRAP(t, spawn_fn("pinger", body));
    KTEST_YIELD_UNTIL(pings == 5);
}

KTEST_CASE(sched_exit_reaps_thread) {
    using kernel::obj::g_type_registry;
    uint32_t before = g_type_registry.live_count(Thread::TYPE_ID);
    auto body       = [] {};
    {
        KTEST_UNWRAP(t, spawn_fn("exiter", body));
        // t drops at scope end; the zombie + reaper path must release the rest.
    }
    KTEST_YIELD_UNTIL(g_type_registry.live_count(Thread::TYPE_ID) <= before);
}

// One spinner fixture drives both halves of the contention story: the spinner only runs if
// preemption works, and its accounting must record the preemptions and the wait latency.
KTEST_CASE(sched_preempts_spinner_and_accounts_latency) {
    volatile uint64_t spin_count = 0;
    volatile bool spin_stop      = false;
    auto body                    = [&] {
        while (!spin_stop) { spin_count = spin_count + 1; }
    };
    KTEST_UNWRAP(t, spawn_fn("spinner", body));
    // Busy-wait WITHOUT yielding: only preemption can give the spinner CPU time.
    ktime_t start = kernel::time::now();
    while (kernel::time::now() < start + 3 * CONFIG_SCHED_TIMESLICE_TICKS) {}
    uint64_t observed = spin_count;
    spin_stop         = true;
    KTEST_EXPECT_TRUE(observed > 0);
    uint32_t sig = t->wait_signals(Thread::SIGNAL_TERMINATED);
    KTEST_EXPECT_TRUE((sig & Thread::SIGNAL_TERMINATED) != 0);
    KTEST_EXPECT_TRUE(t->state() == thread_state::DEAD);
    KTEST_EXPECT_TRUE(t->stats().preemptions >= 1);
    KTEST_EXPECT_TRUE(t->stats().lat_max_cycles > 0);  // waited while main held the CPU
}

KTEST_CASE(sched_sleep_advances_time) {
    ktime_t before = kernel::time::now();
    sleep_ticks(5);
    KTEST_EXPECT_TRUE(kernel::time::now() >= before + 5);
}

KTEST_CASE(sched_block_and_wake) {
    kernel::sched::wait_queue wq;
    volatile int phase = 0;
    auto body          = [&] {
        phase = 1;
        wq.block();
        phase = 2;
    };
    KTEST_UNWRAP(t, spawn_fn("blocker", body));
    KTEST_YIELD_UNTIL(phase == 1);
    KTEST_YIELD_UNTIL(t->state() == thread_state::BLOCKED);
    sleep_ticks(3);  // give it slices; it must stay blocked, not spin
    KTEST_EXPECT_EQUAL(phase, 1);
    KTEST_EXPECT_TRUE(t->state() == thread_state::BLOCKED);
    wq.wake_one();
    KTEST_YIELD_UNTIL(phase == 2);
}

KTEST_CASE(sched_join_via_terminated_signal) {
    auto body = [] { kernel::sched::sleep_ticks(3); };
    KTEST_UNWRAP(t, spawn_fn("mortal", body));
    uint32_t sig = t->wait_signals(Thread::SIGNAL_TERMINATED);
    KTEST_EXPECT_TRUE((sig & Thread::SIGNAL_TERMINATED) != 0);
    KTEST_EXPECT_TRUE(t->state() == thread_state::DEAD);
}

namespace { constexpr uint32_t TEST_SIGNAL = 1u << 3; }  // namespace

// wake_matching (driven here via signal_set) must never wake a mask==0 waiter -- only wake_one may.
KTEST_CASE(sched_mask_zero_ignores_signal_wake) {
    kernel::obj::Event ev;
    volatile int phase = 0;
    auto body          = [&] {
        phase = 1;
        ev.waiters().block(0);
        phase = 2;
    };
    KTEST_UNWRAP(t, spawn_fn("mask0", body));
    KTEST_YIELD_UNTIL(phase == 1);
    KTEST_YIELD_UNTIL(t->state() == thread_state::BLOCKED);
    ev.signal_set(TEST_SIGNAL);
    sleep_ticks(3);  // give it slices; a wrongly-woken waiter would have advanced by now
    KTEST_EXPECT_EQUAL(phase, 1);
    KTEST_EXPECT_TRUE(t->state() == thread_state::BLOCKED);
    ev.waiters().wake_one();
    KTEST_YIELD_UNTIL(phase == 2);
}

// wake_one must never wake a signal (nonzero-mask) waiter -- only wake_matching may.
KTEST_CASE(sched_signal_waiter_ignores_wake_one) {
    kernel::obj::Event ev;
    volatile int phase = 0;
    auto body          = [&] {
        phase = 1;
        ev.wait_signals(TEST_SIGNAL);
        phase = 2;
    };
    KTEST_UNWRAP(t, spawn_fn("sigwait", body));
    KTEST_YIELD_UNTIL(phase == 1);
    KTEST_YIELD_UNTIL(t->state() == thread_state::BLOCKED);
    ev.waiters().wake_one();
    sleep_ticks(3);
    KTEST_EXPECT_EQUAL(phase, 1);
    KTEST_EXPECT_TRUE(t->state() == thread_state::BLOCKED);
    ev.signal_set(TEST_SIGNAL);
    KTEST_YIELD_UNTIL(phase == 2);
}

KTEST_CASE(sched_mutex_blocks_and_wakes) {
    kernel::synchronization::mutex mtx;
    volatile int phase = 0;
    auto body          = [&] {
        kernel::synchronization::lock_guard guard(mtx);
        phase = 2;
    };
    mtx.lock();
    KTEST_UNWRAP(t, spawn_fn("mutex-waiter", body));
    phase = 1;
    KTEST_YIELD_UNTIL(t->state() == thread_state::BLOCKED);
    KTEST_EXPECT_EQUAL(phase, 1);
    mtx.unlock();
    KTEST_YIELD_UNTIL(phase == 2);
}

KTEST_CASE(sched_critical_section_defers_preemption) {
    volatile uint64_t runs = 0;
    volatile bool stop     = false;
    auto body              = [&] {
        while (!stop) { runs = runs + 1; }
    };
    KTEST_UNWRAP(t, spawn_fn("deferred-spinner", body));
    {
        kernel::synchronization::critical_section critical;
        uint64_t before = runs;
        ktime_t until   = kernel::time::now() + 2 * CONFIG_SCHED_TIMESLICE_TICKS;
        while (kernel::time::now() < until) {}
        KTEST_EXPECT_EQUAL(runs, before);
        KTEST_EXPECT_TRUE(kernel::synchronization::current_execution_context().preempt_pending);
    }
    KTEST_EXPECT_TRUE(runs > 0);
    stop = true;
    KTEST_YIELD_UNTIL(t->state() == thread_state::DEAD);
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

KTEST_CASE_CRASH(sched_stack_tripwire) { (void)recurse_forever(0); }

KTEST_CASE(sched_timestamp_monotonic_and_calibrated) {
    uint64_t hz = kernel::platform::timestamp_hz();
    KTEST_EXPECT_TRUE(hz > 0);  // riscv constant; x86 calibrated in late_boot
    uint64_t a = kernel::arch::timestamp();
    uint64_t b = kernel::arch::timestamp();
    KTEST_EXPECT_TRUE(b >= a);
    ktime_t t0 = kernel::time::now();
    while (kernel::time::now() < t0 + 2) {}
    KTEST_EXPECT_TRUE(kernel::arch::timestamp() > a);
}

KTEST_CASE(time_ns_since_boot_is_highres) {
    // late_boot adopted the cycle counter via time::use_timestamp_clock(), so ns_since_boot must
    // advance within a sub-tick window. Spin ~100 us of counter time per round: tick-derived time
    // reports 0 for at least two of the three rounds (at most one can cross a 1 ms tick boundary),
    // so every-round advance proves counter resolution. Preemption can only lengthen a round.
    uint64_t hz = kernel::platform::timestamp_hz();
    KTEST_EXPECT_TRUE(hz > 0);
    for (int round = 0; round < 3; ++round) {
        time_ns_t a    = kernel::time::ns_since_boot();
        uint64_t start = kernel::arch::timestamp();
        while (kernel::arch::timestamp() - start < hz / 10000) {}
        time_ns_t b = kernel::time::ns_since_boot();
        KTEST_EXPECT_TRUE(b - a >= 90'000);
    }
}

KTEST_CASE(sched_accounting_lifecycle_counters) {
    auto body = [] {
        kernel::sched::yield();
        kernel::sched::sleep_ticks(2);
    };
    KTEST_UNWRAP(t, spawn_fn("acct", body));
    t->wait_signals(Thread::SIGNAL_TERMINATED);
    KTEST_EXPECT_TRUE(t->stats().scheduled >= 2);  // initial run + wake from sleep
    KTEST_EXPECT_TRUE(t->stats().yields >= 1);
    KTEST_EXPECT_TRUE(t->stats().sleeps == 1);
    KTEST_EXPECT_TRUE(t->stats().wakes >= 1);      // sleeper wake
    KTEST_EXPECT_TRUE(t->stats().cpu_cycles > 0);  // sub-tick runtime still visible
}

KTEST_CASE(sched_trace_records_thread_life) {
    kernel::sched::trace_clear();
    auto body = [] {};
    KTEST_UNWRAP(t, spawn_fn("traced", body));
    uint64_t tid = t->id();
    t->wait_signals(Thread::SIGNAL_TERMINATED);
    kernel::sched::trace_record recs[64];
    size_t n = kernel::sched::trace_copy_newest(recs, 64);
    KTEST_REQUIRE_TRUE(n > 0);
    bool saw_spawn = false, saw_switch_in = false, saw_exit = false;
    for (size_t i = 0; i < n; ++i) {
        if (recs[i].kind == kernel::sched::trace_kind::SPAWN && recs[i].to_id == tid) { saw_spawn = true; }
        if (recs[i].kind == kernel::sched::trace_kind::SWITCH && recs[i].to_id == tid) { saw_switch_in = true; }
        if (recs[i].kind == kernel::sched::trace_kind::EXIT && recs[i].from_id == tid) { saw_exit = true; }
    }
    KTEST_EXPECT_ALL(saw_spawn, saw_switch_in, saw_exit);
}

KTEST_CASE(sched_global_stats_advance) {
    auto s0 = kernel::sched::stats_snapshot();
    kernel::sched::sleep_ticks(2);
    auto s1 = kernel::sched::stats_snapshot();
    KTEST_EXPECT_TRUE(s1.switches > s0.switches);
    KTEST_EXPECT_TRUE(s1.sleep_switches > s0.sleep_switches);
    KTEST_EXPECT_TRUE(s1.boot_ts == s0.boot_ts);
    KTEST_EXPECT_TRUE(s1.boot_ts != 0);
}

KTEST_CASE(sched_idle_state_truthful_while_switched_out) {
    // We (the kshell thread) are RUNNING, so on a single scheduling core idle must report READY,
    // never a second RUNNING.
    ktl::vector<ktl::ref<Thread>> threads;
    KTEST_REQUIRE_TRUE(kernel::sched::kernel_task()->snapshot_threads(threads));
    bool found_idle = false;
    for (size_t i = 0; i < threads.size(); ++i) {
        if (threads[i]->name() != nullptr && ktl::string_view(threads[i]->name()) == "idle0") {
            found_idle = true;
            KTEST_EXPECT_TRUE(threads[i]->state() == thread_state::READY);
        }
    }
    KTEST_REQUIRE_TRUE(found_idle);
}
