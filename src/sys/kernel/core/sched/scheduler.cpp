// src/sys/kernel/core/sched/scheduler.cpp
#include <kernel/arch.h>
#include <kernel/assert.h>
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

// Stack tripwire floor for the running thread, consumed by the arch trap entries. Published on
// every switch. Zero disables the check (x86 idle; pre-scheduler riscv boot writes a real floor
// from trap_init).
extern "C" uintptr_t g_kstack_floor = 0;

namespace kernel::sched {

// All scheduler state below is guarded by interrupts-off on the single scheduling core.
cpu_sched g_cpus[CONFIG_MAX_CORES];
uint32_t g_boot_core = 0;

cpu_sched& cur_cpu() { return g_cpus[g_boot_core]; }

namespace {

bool g_started = false;

// Interrupts must be disabled. Returns when this thread is next switched in.
void switch_to(ktl::ref<Thread> next, switch_reason reason) {
    assert(!kernel::synchronization::preemption_disabled(), "switch_to: preemption disabled");
    auto& c      = cur_cpu();
    uint64_t now = kernel::arch::timestamp();
    c.current->stats().cpu_cycles += now - g_last_switch_ts;
    g_last_switch_ts = now;

    next->stats().scheduled += 1;
    if (next->ready_ts() != 0) {
        uint64_t lat = now - next->ready_ts();
        next->stats().lat_total_cycles += lat;
        if (lat > next->stats().lat_max_cycles) { next->stats().lat_max_cycles = lat; }
        next->set_ready_ts(0);
    }

    g_stats.switches += 1;
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

    Thread* outgoing = c.current.get();
#ifndef NDEBUG
    auto& execution = kernel::synchronization::current_execution_context();
    outgoing->set_held_lock_count(execution.held_count);
    for (size_t i = 0; i < execution.held_count; ++i) { outgoing->held_locks()[i] = execution.held[i]; }
#endif
    // Callers park or re-queue the outgoing thread before switching; only the idle handoff paths
    // leave it RUNNING (idle is never re-queued), so demote it here to keep reported state truthful.
    if (outgoing->state() == thread_state::RUNNING) { outgoing->set_state(thread_state::READY); }
    next->set_state(thread_state::RUNNING);
    next->reset_slice();
    assert(c.previous.get() == nullptr, "switch_to: unfinished previous switch");
    c.previous = ktl::move(c.current);
    c.current  = ktl::move(next);
    kernel::synchronization::set_current_thread_id(c.current->id());
#ifndef NDEBUG
    execution.held_count = c.current->held_lock_count();
    for (size_t i = 0; i < execution.held_count; ++i) { execution.held[i] = c.current->held_locks()[i]; }
#endif
    g_kstack_floor  = c.current->kstack_floor();
    auto* next_task = static_cast<Task*>(c.current->owner().get());
    if (next_task != nullptr && next_task->aspace() != nullptr &&
        kernel::mm::vm_aspace::active() != next_task->aspace()) {
        next_task->aspace()->activate();
    }
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

}  // namespace

bool started() { return g_started; }

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
    size_t live    = g_live_threads.fetch_add(1, ktl::memory_order::relaxed) + 1;
    // +2 is slack, not accounting: the boot context registers as the idle thread without passing
    // through spawn (idle is handed off directly, but a requeue path touching it must not be the
    // thing that discovers the queue is exactly full), plus one for off-by-one churn while a
    // spawn and a reap race the counter. The reservations only need to be >= the worst case.
    size_t target  = live + 2;

    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    bool ok        = cur_cpu().run_queue.reserve(target);
    kernel::arch::restore_interrupts(flags);
    if (ok) { ok = sleepers_reserve(target); }
    if (ok) { ok = zombies_reserve(target); }
    if (!ok) {
        g_live_threads.fetch_sub(1, ktl::memory_order::relaxed);
        return ktl::err(ktl::errc::oom);
    }
    return ktl::result<void>::ok();
}

void note_thread_reaped() { g_live_threads.fetch_sub(1, ktl::memory_order::relaxed); }

void make_ready(ktl::ref<Thread> thread) {
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    assert(thread->state() == thread_state::BLOCKED, "make_ready: thread is not blocked");
    thread->set_state(thread_state::READY);
    thread->set_ready_ts(kernel::arch::timestamp());
    thread->stats().wakes += 1;
    g_stats.wakes += 1;
    trace_push(trace_kind::WAKE, switch_reason::NONE, cur_cpu().current ? cur_cpu().current->id() : 0, thread->id());
    bool ok = cur_cpu().run_queue.push_back(ktl::move(thread));
    ensure(ok, "make_ready: run queue push failed despite reservation");
    kernel::arch::restore_interrupts(flags);
}

void schedule_out(switch_reason reason) {
    kernel::synchronization::assert_blocking_allowed("schedule_out: scheduling is forbidden in this context");
    kernel::synchronization::assert_no_locks_held("schedule_out: scheduling while holding a lock");
    auto& c   = cur_cpu();
    auto next = c.run_queue.pop_front();
    switch_to(next.has_value() ? ktl::move(*next) : c.idle, reason);
}

void yield() {
    kernel::synchronization::assert_blocking_allowed("yield: scheduling is forbidden in this context");
    kernel::synchronization::assert_no_locks_held("yield: scheduling while holding a lock");
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    auto& c        = cur_cpu();
    auto next      = c.run_queue.pop_front();
    if (!next.has_value() && c.current.get() != c.idle.get()) {
        // Queue empty: hand off to idle. The boot context runs as the idle thread and is never
        // re-queued after a tick preemption, so a cooperatively-yielding thread would otherwise
        // starve it forever.
        next = c.idle;
    }
    if (next.has_value()) {
        if (c.current.get() != c.idle.get()) {
            c.current->stats().yields += 1;
            c.current->set_state(thread_state::READY);
            c.current->set_ready_ts(kernel::arch::timestamp());
            bool ok = c.run_queue.push_back(c.current);
            ensure(ok, "yield: run queue push failed despite reservation");
        }
        switch_to(ktl::move(*next), switch_reason::YIELD);
    }
    kernel::arch::restore_interrupts(flags);
}

void on_tick() {
    if (!g_started) { return; }
    auto& c = cur_cpu();
    // Wake due sleepers and expired timed waits first so a woken thread can be this tick's
    // switch target.
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
    if (c.run_queue.size() == 0) { return; }
    if (!idle_running && c.current->decrement_slice() > 0) { return; }
    kernel::synchronization::request_preemption();
}

void service_pending_preemption() {
    if (!g_started) { return; }
    // switch_to must run with interrupts masked (see yield/schedule_out). This hook is reached from
    // preempt_enable on the plain critical_section path, where interrupts are still enabled, so an
    // unmasked switch could be re-entered by a timer tick mid-switch. Nesting-safe for the already-
    // masked interrupt_exit/fault_exit callers.
    uint64_t flags = kernel::arch::save_and_disable_interrupts();
    auto& c        = cur_cpu();
    // A killed thread that never syscalls would otherwise run user code forever between ticks.
    // Only when no syscall is in flight: mid-syscall the thread may hold kernel locks it must
    // release on its way out, and the dispatcher's own boundary check exits it lock-free. With
    // syscall depth zero and this hook eligible to run, the interrupted context is user code,
    // which holds nothing.
    if (c.current->killed() && c.current.get() != c.idle.get() &&
        kernel::synchronization::current_execution_context().syscall_depth == 0) {
        exit_current();
    }
    if (c.run_queue.size() == 0) {
        kernel::arch::restore_interrupts(flags);
        return;
    }
    auto next         = c.run_queue.pop_front();
    bool idle_running = (c.current.get() == c.idle.get());
    if (!idle_running) {
        c.current->stats().preemptions += 1;
        c.current->set_state(thread_state::READY);
        c.current->set_ready_ts(kernel::arch::timestamp());
        bool ok = c.run_queue.push_back(c.current);
        ensure(ok, "service_pending_preemption: run queue push failed despite reservation");
    }
    switch_to(ktl::move(*next), switch_reason::PREEMPT);
    kernel::arch::restore_interrupts(flags);
}

[[noreturn]] void exit_current() {
    kernel::synchronization::assert_blocking_allowed("exit_current: scheduling is forbidden in this context");
    kernel::synchronization::assert_no_locks_held("exit_current: exiting while holding a lock");
    if (lifecycle_log_enabled()) { g_log.debug("sched: exit id={0}", current()->id()); }
    kernel::arch::disable_interrupts();
    auto& c = cur_cpu();
    assert(c.current.get() != c.idle.get(), "exit_current: idle thread cannot exit");
    c.current->set_state(thread_state::DEAD);
    c.current->signal_set(Thread::SIGNAL_TERMINATED);
    trace_push(trace_kind::EXIT, switch_reason::NONE, c.current->id(), 0);
    reaper_enqueue(c.current);
    schedule_out(switch_reason::EXIT);
    panic("exit_current: switched back into a dead thread");
}

void init(uint32_t boot_core_index) {
    g_boot_core = boot_core_index;
    // The boot context becomes the idle thread: already running, and its stack keeps whatever
    // tripwire the arch established (riscv trap_init; 0 on x86).
    auto idle   = ktl::make_ref<Thread>("idle0", kernel_task(), g_kstack_floor);
    assert(static_cast<bool>(idle), "sched: idle thread allocation failed");
    kernel_task()->add_thread(idle).expect("sched: task zero rejected the idle thread");
    auto& c   = cur_cpu();
    c.idle    = idle;
    c.current = ktl::move(idle);
    kernel::synchronization::set_current_thread_id(c.current->id());
    kernel::synchronization::set_deferred_preempt_hook(service_pending_preemption);
    g_last_switch_ts = kernel::arch::timestamp();
    g_stats.boot_ts  = g_last_switch_ts;
    g_started        = true;
    reaper_start();
    g_log.info("sched: online (core {0})", boot_core_index);
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
extern "C" void sched_finish_switch() { kernel::sched::cur_cpu().previous.reset(); }
extern "C" [[noreturn]] void sched_thread_exit() { kernel::sched::exit_current(); }
