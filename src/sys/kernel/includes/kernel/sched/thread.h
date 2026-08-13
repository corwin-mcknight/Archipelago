#pragma once

#include <kernel/config.h>
#include <kernel/obj/object.h>
#include <kernel/obj/type_registry.h>
#include <kernel/obj/types.h>
#include <kernel/sched/ipc_buffer.h>
#include <kernel/synchronization/execution_context.h>
#include <std/new.h>

#include <ktl/ref>
#include <ktl/result>

namespace kernel::sched {

using thread_entry_fn = void (*)(void*);

enum class thread_state : uint32_t { READY = 0, RUNNING, BLOCKED, DEAD };

// Per-thread scheduler accounting. Plain data: mutated only by the scheduler with interrupts
// disabled on the scheduling core; read by the shell via snapshots.
struct thread_stats {
    uint64_t cpu_cycles       = 0;
    uint64_t scheduled        = 0;
    uint64_t preemptions      = 0;
    uint64_t yields           = 0;
    uint64_t blocks           = 0;
    uint64_t sleeps           = 0;
    uint64_t wakes            = 0;
    uint64_t lat_total_cycles = 0;
    uint64_t lat_max_cycles   = 0;
};

class Thread : public kernel::obj::Object {
   public:
    DECLARE_OBJECT_TYPE(Thread, kernel::obj::type_ids::THREAD)
    static constexpr uint32_t SIGNAL_TERMINATED = 1u << 0;

    // Bare thread: no name, no owner, no stack. Kernel code uses one of the two constructors
    // below; this exists for tests exercising Thread/Task bookkeeping in isolation.
    // Threads allocate from their AUMI arena (exact-size zeroed slots) rather than the general
    // heap. Defined in mm/object_arena.cpp beside the arena so hosted builds link them without
    // building this class's kernel-only translation units.
    static void* operator new(size_t size, const std::nothrow_t&) noexcept;
    static void operator delete(void* ptr);

    Thread() : Object(TYPE_ID) {}

    // Adopting constructor: wraps an already-running context (the boot/idle thread). It owns no
    // stack, and the context is already executing, so it starts RUNNING. kstack_floor is whatever
    // tripwire the arch established for that stack (0 disables the check).
    Thread(const char* name, ktl::ref<kernel::obj::Object> owner, uintptr_t kstack_floor)
        : Object(TYPE_ID), m_state(thread_state::RUNNING), m_kstack_floor(kstack_floor), m_owner(ktl::move(owner)) {
        set_name(name);
    }

    // Spawned thread: owns the stack identified by its physical base and its HHDM-mapped virtual
    // base. Starts READY; the scheduler prepares the initial switch frame and records it via
    // set_saved_sp().
    Thread(const char* name, ktl::ref<kernel::obj::Object> owner, uintptr_t kstack_phys, uintptr_t kstack_virt_base)
        : Object(TYPE_ID),
          m_kstack_phys(kstack_phys),
          m_kstack_floor(kstack_virt_base + CONFIG_KERNEL_STACK_TRIPWIRE_MARGIN),
          m_kstack_top(kstack_virt_base + CONFIG_KERNEL_STACK_SIZE),
          m_owner(ktl::move(owner)) {
        set_name(name);
    }

    thread_state state() const { return m_state; }
    void set_state(thread_state s) { m_state = s; }

    // Set once by task kill, never cleared. A killed thread is forced onto the exit path at its
    // next kernel boundary: the syscall dispatcher and the preemption service check it, blocked
    // threads are woken by the killer, and the wait primitives refuse to park it on a wait whose
    // wake might never come (see wait_queue.cpp).
    bool killed() const { return m_killed; }
    void set_killed() { m_killed = true; }

    // Where this thread is parked while BLOCKED on a wait queue, recorded under that queue's lock
    // by block_if and cleared when the thread resumes. This is what lets task kill find and claim
    // a blocked thread without a reverse index; sleepers are found through the sleeper list
    // instead and have no entry here.
    wait_queue* parked_queue() const { return m_parked_queue; }
    wait_node* parked_node() const { return m_parked_node; }
    void set_parked(wait_queue* queue, wait_node* node) {
        m_parked_queue = queue;
        m_parked_node  = node;
    }

    uintptr_t kstack_phys() const { return m_kstack_phys; }
    uintptr_t kstack_floor() const { return m_kstack_floor; }
    uintptr_t kstack_top() const { return m_kstack_top; }

    ktl::ref<kernel::obj::Object>& owner() { return m_owner; }

    uintptr_t saved_sp() const { return m_saved_sp; }
    uintptr_t* saved_sp_slot() { return &m_saved_sp; }
    void set_saved_sp(uintptr_t sp) { m_saved_sp = sp; }

    void reset_slice() { m_slice = CONFIG_SCHED_TIMESLICE_TICKS; }
    // Returns the remaining slice after the decrement; saturates at zero.
    uint32_t decrement_slice() {
        if (m_slice > 0) { --m_slice; }
        return m_slice;
    }

    thread_stats& stats() { return m_stats; }
    const thread_stats& stats() const { return m_stats; }
#ifndef NDEBUG
    kernel::synchronization::held_lock* held_locks() { return m_held_locks; }
    size_t held_lock_count() const { return m_held_lock_count; }
    void set_held_lock_count(size_t count) { m_held_lock_count = count; }
#endif

    // Timestamp of the last enqueue onto the run queue; 0 when not pending. Set at every
    // enqueue, consumed (and zeroed) when the thread is switched in -- READY latency.
    uint64_t ready_ts() const { return m_ready_ts; }
    void set_ready_ts(uint64_t ts) { m_ready_ts = ts; }

    // The only memory a syscall reads from this thread. Invalid for kernel threads, which have no
    // address space to map one into and make no syscalls.
    const ipc_buffer& ipc() const { return m_ipc; }
    void set_ipc(ipc_buffer buffer) { m_ipc = ktl::move(buffer); }

    static ktl::result<void> register_type(kernel::obj::TypeRegistry& registry) {
        using namespace kernel::obj;
        return registry.register_type(TYPE_ID, "thread", RIGHT_READ | RIGHT_WAIT | RIGHT_DUPLICATE,
                                      RIGHT_READ | RIGHT_WAIT);
    }

   private:
    // Mutated only with interrupts disabled on the scheduling core; no atomics until SMP scheduling.
    thread_state m_state       = thread_state::READY;
    bool m_killed              = false;
    wait_queue* m_parked_queue = nullptr;
    wait_node* m_parked_node   = nullptr;
    uintptr_t m_kstack_phys    = 0;
    uintptr_t m_kstack_floor   = 0;
    uintptr_t m_kstack_top     = 0;
    uintptr_t m_saved_sp       = 0;
    ktl::ref<kernel::obj::Object> m_owner;
    uint32_t m_slice = CONFIG_SCHED_TIMESLICE_TICKS;
    thread_stats m_stats;
    uint64_t m_ready_ts = 0;
    ipc_buffer m_ipc;
#ifndef NDEBUG
    kernel::synchronization::held_lock m_held_locks[CONFIG_LOCKDEP_MAX_HELD] = {};
    size_t m_held_lock_count                                                 = 0;
#endif
};

}  // namespace kernel::sched
