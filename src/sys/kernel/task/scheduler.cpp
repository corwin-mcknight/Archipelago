// src/sys/kernel/task/scheduler.cpp
#include <kernel/arch.h>
#include <kernel/assert.h>
#include <kernel/boot.h>
#include <kernel/config.h>
#include <kernel/log.h>
#include <kernel/mm/vm_aspace.h>
#include <kernel/panic.h>
#include <kernel/sched/internal.h>
#include <kernel/sched/reaper.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/task.h>
#include <kernel/synchronization/execution_context.h>

#include <ktl/atomic>

namespace kernel::sched {

[[clang::no_destroy]] kernel::synchronization::spinlock g_sched_lock;

namespace { [[clang::no_destroy]] cpu_sched g_cpus[CONFIG_MAX_CORES]; }

cpu_sched& cur_cpu() { return g_cpus[kernel::arch::current_core_index()]; }
cpu_sched& cpu_at(size_t index) { return g_cpus[index]; }
// From the boot protocol rather than init(): the boot core's timer ticks (and advances kernel
// time) before the scheduler exists.
bool on_boot_core() { return kernel::arch::current_core_index() == kernel::boot::collect().boot_cpu_index; }

namespace {

ktl::atomic<bool> g_started{false};

// One FIFO under g_sched_lock; any core picks from it.
// ponytail: one shared queue, per-core queues if the lock shows up in profiles.
[[clang::no_destroy]] ktl::deque<ktl::ref<Thread>> g_run_queue;

// Pop the first thread whose previous core has finished switching it out; one still on-cpu rotates
// to the back so no core ever waits on another inside the lock.
ktl::maybe<ktl::ref<Thread>> pop_locked(ktl::deque<ktl::ref<Thread>>& queue) {
    for (size_t n = queue.size(); n > 0; --n) {
        auto thread = queue.pop_front();
        if (!(*thread)->on_cpu()) { return thread; }
        bool ok = queue.push_back(ktl::move(*thread));
        ensure(ok, "pick: run queue push failed despite reservation");
    }
    return ktl::maybe<ktl::ref<Thread>>();
}

ktl::maybe<ktl::ref<Thread>> pick_next_locked() { return pop_locked(g_run_queue); }

bool has_runnable_locked() { return g_run_queue.size() != 0; }

// A core parked in wait_for_interrupt() would otherwise notice new work only at its next tick. An
// idle pusher (the tick waking a sleeper) picks the thread up itself at interrupt exit, so it
// kicks nobody.
void kick_idle_core_locked() {
    size_t self = kernel::arch::current_core_index();
    if (g_cpus[self].current.get() == g_cpus[self].idle.get()) { return; }
    for (size_t i = 0; i < CONFIG_MAX_CORES; i++) {
        auto& c = g_cpus[i];
        if (i == self || !c.current || c.current.get() != c.idle.get()) { continue; }
        kernel::arch::send_reschedule_ipi(i);
        return;
    }
}

// Interrupts must be disabled and no lock held. Returns when this thread is next switched in.
void switch_to(ktl::ref<Thread> next, switch_reason reason) {
    assert(!kernel::synchronization::preemption_disabled(), "switch_to: preemption disabled");
    auto& c          = cur_cpu();
    Thread* outgoing = c.current.get();
    auto& execution  = kernel::synchronization::current_execution_context();
    outgoing->set_syscall_depth(execution.syscall_depth);
#ifndef NDEBUG
    outgoing->set_held_lock_count(execution.held_count);
    for (size_t i = 0; i < execution.held_count; ++i) { outgoing->held_locks()[i] = execution.held[i]; }
#endif
    {
        sched_guard guard(g_sched_lock);
        uint64_t now = kernel::arch::timestamp();
        c.current->stats().cpu_cycles += now - c.last_switch_ts;
        c.last_switch_ts = now;

        next->stats().scheduled += 1;
        uint32_t core = (uint32_t)kernel::arch::current_core_index();
        if (next->stats().last_core != core && next->stats().last_core != thread_stats::NO_CORE) {
            next->stats().migrations += 1;
        }
        next->stats().last_core = core;
        if (next->ready_ts() != 0) {
            uint64_t lat = now - next->ready_ts();
            next->stats().lat_total_cycles += lat;
            if (lat > next->stats().lat_max_cycles) { next->stats().lat_max_cycles = lat; }
            next->set_ready_ts(0);
        }

        g_stats.switches += 1;
        c.switches += 1;
        switch (reason) {
            case switch_reason::PREEMPT: g_stats.preempts += 1; break;
            case switch_reason::YIELD: g_stats.yields += 1; break;
            case switch_reason::BLOCK: g_stats.block_switches += 1; break;
            case switch_reason::SLEEP: g_stats.sleep_switches += 1; break;
            case switch_reason::EXIT: g_stats.exit_switches += 1; break;
            case switch_reason::NONE:
            default: break;
        }
        trace_push(trace_kind::SWITCH, reason, c.current->id(), next->id());

        // Callers park or re-queue the outgoing thread before switching; only the idle handoff
        // paths leave it RUNNING (idle is never re-queued), so demote it here to keep reported
        // state truthful.
        if (outgoing->state() == thread_state::RUNNING) { outgoing->set_state(thread_state::READY); }
        next->set_state(thread_state::RUNNING);
        next->reset_slice();
        next->set_on_cpu(true);
        assert(c.previous.get() == nullptr, "switch_to: unfinished previous switch");
        c.previous = ktl::move(c.current);
        c.current  = ktl::move(next);
    }
    // After the unlock: lockdep attributes the lock to the thread id at acquire and checks it at
    // release, so the identity changes hands only once nothing is held.
    kernel::synchronization::set_current_thread_id(c.current->id());
    execution.syscall_depth = c.current->syscall_depth();
#ifndef NDEBUG
    execution.held_count = c.current->held_lock_count();
    for (size_t i = 0; i < execution.held_count; ++i) { execution.held[i] = c.current->held_locks()[i]; }
#endif
    kernel::arch::set_kstack_floor(c.current->kstack_floor());
    // A core holds a user address space active only while running one of that task's threads, so
    // the reaper can tear a space down from any core once its last thread is off-cpu.
    auto* next_task = static_cast<Task*>(c.current->owner().get());
    auto* want =
        next_task != nullptr && next_task->aspace() != nullptr ? next_task->aspace() : &kernel::mm::kernel_aspace();
    if (kernel::mm::vm_aspace::active() != want) { want->activate(); }
    if (c.current->kstack_top() != 0) { kernel::arch::set_kernel_stack(c.current->kstack_top()); }
    // User FP/SIMD state changes hands only here, and only user threads (task with an address
    // space) have any: kernel code never touches those registers, so whatever user mode left in
    // them survives every kernel entry -- and every kernel-thread stretch -- until the next user
    // thread is restored.
    auto* prev_task = static_cast<Task*>(outgoing->owner().get());
    if (prev_task != nullptr && prev_task->aspace() != nullptr) { kernel::arch::fpu_save(outgoing->fpu_area()); }
    if (next_task != nullptr && next_task->aspace() != nullptr) { kernel::arch::fpu_restore(c.current->fpu_area()); }
    arch_context_switch(outgoing->saved_sp_slot(), c.current->saved_sp());
    sched_finish_switch();
}

// Idle threads are created up front by init() on the boot core: registering one with task zero
// takes a mutex, which a core that is not yet a thread cannot block on.
void create_idle(uint32_t core_index) {
    static char names[CONFIG_MAX_CORES][8];
    char* name = names[core_index];
    name[0]    = 'i';
    name[1]    = 'd';
    name[2]    = 'l';
    name[3]    = 'e';
    size_t n   = 4;
    if (core_index >= 10) { name[n++] = (char)('0' + core_index / 10); }
    name[n++] = (char)('0' + core_index % 10);
    name[n]   = '\0';

    auto idle = ktl::make_ref<Thread>(name, kernel_task(), uintptr_t{0});
    assert(static_cast<bool>(idle), "sched: idle thread allocation failed");
    kernel_task()->add_thread(idle).expect("sched: task zero rejected the idle thread");
    idle->set_state(thread_state::READY);
    g_cpus[core_index].idle = ktl::move(idle);
}

// The calling context becomes its core's idle thread: already running, on a stack whose tripwire
// the arch established.
void adopt_idle() {
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    auto& c        = cur_cpu();
    assert(static_cast<bool>(c.idle), "sched: no idle thread for this core");
    c.idle->set_kstack_floor(kernel::arch::kstack_floor());
    c.idle->stats().last_core = (uint32_t)kernel::arch::current_core_index();
    c.idle->set_state(thread_state::RUNNING);
    c.idle->set_on_cpu(true);
    c.current = c.idle;
    kernel::synchronization::set_current_thread_id(c.current->id());
    c.last_switch_ts = kernel::arch::timestamp();
    kernel::arch::restore_interrupts(flags);
}

}  // namespace

bool started() { return g_started.load(ktl::memory_order::acquire); }

ktl::ref<Thread> current() {
    uint64_t flags     = kernel::arch::save_and_disable_interrupts();
    ktl::ref<Thread> t = cur_cpu().current;
    kernel::arch::restore_interrupts(flags);
    return t;
}

bool current_is_idle() {
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    auto& c        = cur_cpu();
    bool idle      = c.current.get() == c.idle.get();
    kernel::arch::restore_interrupts(flags);
    return idle;
}

namespace {
// Live threads known to scheduling; the high-water mark of this count is what the run queue and
// sleeper list stay reserved for, so tick-context pushes never grow them.
ktl::atomic<size_t> g_live_threads{0};
}  // namespace

ktl::result<void> ensure_tick_capacity() {
    size_t live   = g_live_threads.fetch_add(1, ktl::memory_order::relaxed) + 1;
    // +2 is slack, not accounting: the boot context registers as the idle thread without passing
    // through spawn (idle is handed off directly, but a requeue path touching it must not be the
    // thing that discovers the queue is exactly full), plus one for off-by-one churn while a
    // spawn and a reap race the counter. The reservations only need to be >= the worst case.
    size_t target = live + 2;

    bool ok       = grow_under_sched_lock(g_run_queue, target);
    if (ok) { ok = sleepers_reserve(target); }
    if (ok) { ok = zombies_reserve(target); }
    if (!ok) {
        g_live_threads.fetch_sub(1, ktl::memory_order::relaxed);
        return ktl::err(ktl::errc::oom);
    }
    return ktl::result<void>::ok();
}

void note_thread_reaped() { g_live_threads.fetch_sub(1, ktl::memory_order::relaxed); }

bool push_runnable_locked(ktl::ref<Thread> thread) {
    if (!g_run_queue.push_back(ktl::move(thread))) { return false; }
    kick_idle_core_locked();
    return true;
}

size_t run_queue_depth_locked() { return g_run_queue.size(); }

void make_ready_locked(ktl::ref<Thread> thread) {
    assert(thread->state() == thread_state::BLOCKED, "make_ready: thread is not blocked");
    thread->set_state(thread_state::READY);
    thread->set_ready_ts(kernel::arch::timestamp());
    thread->stats().wakes += 1;
    g_stats.wakes += 1;
    auto& c = cur_cpu();
    trace_push(trace_kind::WAKE, switch_reason::NONE, c.current ? c.current->id() : 0, thread->id());
    bool ok = push_runnable_locked(ktl::move(thread));
    ensure(ok, "make_ready: run queue push failed despite reservation");
}

void make_ready(ktl::ref<Thread> thread) {
    sched_guard guard(g_sched_lock);
    make_ready_locked(ktl::move(thread));
}

void schedule_out(switch_reason reason) {
    kernel::synchronization::assert_blocking_allowed("schedule_out: scheduling is forbidden in this context");
    kernel::synchronization::assert_no_locks_held("schedule_out: scheduling while holding a lock");
    ktl::ref<Thread> next;
    {
        sched_guard guard(g_sched_lock);
        auto picked = pick_next_locked();
        next        = picked.has_value() ? ktl::move(*picked) : cur_cpu().idle;
    }
    switch_to(ktl::move(next), reason);
}

void yield() {
    kernel::synchronization::assert_blocking_allowed("yield: scheduling is forbidden in this context");
    kernel::synchronization::assert_no_locks_held("yield: scheduling while holding a lock");
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    ktl::ref<Thread> next;
    {
        sched_guard guard(g_sched_lock);
        auto& c = cur_cpu();
        if (c.current.get() != c.idle.get()) { c.current->stats().yields += 1; }
        // Nothing else runnable: keep going. Bouncing through idle would requeue this thread, kick
        // another core for it, and switch twice to land back here.
        auto picked = pick_next_locked();
        if (picked.has_value()) {
            if (c.current.get() != c.idle.get()) {
                c.current->set_state(thread_state::READY);
                c.current->set_ready_ts(kernel::arch::timestamp());
                bool ok = push_runnable_locked(c.current);
                ensure(ok, "yield: run queue push failed despite reservation");
            }
            next = ktl::move(*picked);
        }
    }
    if (next) { switch_to(ktl::move(next), switch_reason::YIELD); }
    kernel::arch::restore_interrupts(flags);
}

void on_tick() {
    if (!g_started.load(ktl::memory_order::acquire)) { return; }
    sched_guard guard(g_sched_lock);
    auto& c = cur_cpu();
    // Wake due sleepers and expired timed waits first so a woken thread can be this tick's
    // switch target. Every core's tick runs the scans; the lists are shared.
    wake_due_sleepers();
    wake_due_timed_waits();
    bool idle_running = (c.current.get() == c.idle.get());
    // A killed thread gets the preemption request even with an empty run queue: the backstop in
    // service_pending_preemption is what stops a killed compute loop that never syscalls, and
    // gating it on other runnable work would let the loop run until something else woke up.
    if (!idle_running && c.current->killed()) {
        kernel::synchronization::request_preemption();
        return;
    }
    if (!has_runnable_locked()) { return; }
    if (!idle_running && c.current->decrement_slice() > 0) { return; }
    kernel::synchronization::request_preemption();
}

void service_pending_preemption() {
    if (!g_started.load(ktl::memory_order::acquire)) { return; }
    // switch_to must run with interrupts masked (see yield/schedule_out). This hook is reached from
    // preempt_enable on the plain critical_section path, where interrupts are still enabled, so an
    // unmasked switch could be re-entered by a timer tick mid-switch. Nesting-safe for the already-
    // masked interrupt_exit/fault_exit callers.
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    ktl::ref<Thread> next;
    bool exit_now = false;
    {
        sched_guard guard(g_sched_lock);
        auto& c  = cur_cpu();
        // A killed thread that never syscalls would otherwise run user code forever between ticks.
        // Only when no syscall is in flight: mid-syscall the thread may hold kernel locks it must
        // release on its way out, and the dispatcher's own boundary check exits it lock-free. With
        // syscall depth zero and this hook eligible to run, the interrupted context is user code,
        // which holds nothing.
        exit_now = c.current->killed() && c.current.get() != c.idle.get() &&
                   kernel::synchronization::current_execution_context().syscall_depth == 0;
        if (!exit_now) {
            auto picked = pick_next_locked();
            if (picked.has_value()) {
                if (c.current.get() != c.idle.get()) {
                    c.current->stats().preemptions += 1;
                    c.current->set_state(thread_state::READY);
                    c.current->set_ready_ts(kernel::arch::timestamp());
                    bool ok = push_runnable_locked(c.current);
                    ensure(ok, "service_pending_preemption: run queue push failed despite reservation");
                }
                next = ktl::move(*picked);
            }
        }
    }
    if (exit_now) { exit_current(); }
    if (next) { switch_to(ktl::move(next), switch_reason::PREEMPT); }
    kernel::arch::restore_interrupts(flags);
}

[[noreturn]] void exit_current() {
    kernel::synchronization::assert_blocking_allowed("exit_current: scheduling is forbidden in this context");
    kernel::synchronization::assert_no_locks_held("exit_current: exiting while holding a lock");
    if (lifecycle_log_enabled()) { g_log.debug("sched: exit id={0}", current()->id()); }
    kernel::arch::disable_interrupts();
    ktl::ref<Thread> self = current();
    {
        sched_guard guard(g_sched_lock);
        auto& c = cur_cpu();
        assert(c.current.get() != c.idle.get(), "exit_current: idle thread cannot exit");
        c.current->set_state(thread_state::DEAD);
        trace_push(trace_kind::EXIT, switch_reason::NONE, c.current->id(), 0);
        reaper_enqueue(c.current);
    }
    // Wakers take the scheduler lock themselves, so both run after it is released. The reaper and
    // any joiner may run on another core immediately; both wait for this core's switch to finish
    // (Thread::on_cpu) before they touch the stack we are still on.
    self->signal_set(Thread::SIGNAL_TERMINATED);
    reaper_kick();
    self.reset();
    schedule_out(switch_reason::EXIT);
    panic("exit_current: switched back into a dead thread");
}

void init(uint32_t boot_core_index) {
    assert(boot_core_index == kernel::boot::collect().boot_cpu_index, "sched: init on a non-boot core");
    size_t cores = kernel::boot::collect().cpu_count;
    if (cores > CONFIG_MAX_CORES) { cores = CONFIG_MAX_CORES; }
    for (uint32_t i = 0; i < cores; i++) { create_idle(i); }
    adopt_idle();
    kernel::synchronization::set_deferred_preempt_hook(service_pending_preemption);
    g_stats.boot_ts = kernel::arch::timestamp();
    g_started.store(true, ktl::memory_order::release);
    reaper_start();
    g_log.info("sched: online (core {0})", boot_core_index);
}

void join_secondary(uint32_t core_index) {
    assert(g_started.load(ktl::memory_order::acquire), "join_secondary: scheduler not started");
    adopt_idle();
    g_log.info("sched: core {0} online", core_index);
}

[[noreturn]] void idle_loop() {
    // Tick preemption (on_tick) switches threads in and out automatically, but idle itself is
    // never preempted into -- it has to cooperatively yield to hand the CPU to a newly-ready
    // thread. yield() is a fast no-op when the run queue is empty, so this still spends most of
    // its time halted in-between.
    while (true) {
        yield();
        kernel::arch::wait_for_interrupt();
    }
}

}  // namespace kernel::sched

// Trampoline hooks. cur_cpu() is file-scope state above, so these live in this file.
extern "C" void sched_finish_switch() {
    auto& c                         = kernel::sched::cur_cpu();
    kernel::sched::Thread* outgoing = c.previous.get();
    c.previous.reset();
    // Last touch: once clear, another core may run the thread, or the reaper may free it.
    if (outgoing != nullptr) { outgoing->set_on_cpu(false); }
}
extern "C" [[noreturn]] void sched_thread_exit() { kernel::sched::exit_current(); }
