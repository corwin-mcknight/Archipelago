#pragma once

#include <kernel/config.h>
#include <kernel/obj/object.h>
#include <kernel/obj/type_registry.h>
#include <kernel/obj/types.h>

#include <ktl/result>

namespace kernel::sched {

using thread_entry_fn = void (*)(void*);

enum class thread_state : uint32_t { READY = 0, RUNNING, BLOCKED, DEAD };

class Thread : public kernel::obj::Object {
   public:
    DECLARE_OBJECT_TYPE(Thread, kernel::obj::type_ids::THREAD)
    static constexpr uint32_t SIGNAL_TERMINATED = 1u << 0;

    // Adopting constructor: wraps an already-running context (the boot/idle thread); no owned stack.
    Thread() : Object(TYPE_ID) {}

    // Spawned thread. The stack is identified by its physical base and its HHDM-mapped virtual
    // base; the scheduler prepares the initial switch frame and records it via set_saved_sp().
    Thread(uintptr_t kstack_phys, uintptr_t kstack_virt_base)
        : Object(TYPE_ID),
          m_kstack_phys(kstack_phys),
          m_kstack_floor(kstack_virt_base + CONFIG_KERNEL_STACK_TRIPWIRE_MARGIN) {}

    thread_state state() const { return m_state; }
    void set_state(thread_state s) { m_state = s; }

    uintptr_t kstack_phys() const { return m_kstack_phys; }
    uintptr_t kstack_floor() const { return m_kstack_floor; }
    void set_kstack_floor(uintptr_t floor) { m_kstack_floor = floor; }

    uintptr_t saved_sp() const { return m_saved_sp; }
    uintptr_t* saved_sp_slot() { return &m_saved_sp; }
    void set_saved_sp(uintptr_t sp) { m_saved_sp = sp; }

    void reset_slice() { m_slice = CONFIG_SCHED_TIMESLICE_TICKS; }
    // Returns the remaining slice after the decrement; saturates at zero.
    uint32_t decrement_slice() {
        if (m_slice > 0) { --m_slice; }
        return m_slice;
    }

    static ktl::result<void> register_type(kernel::obj::TypeRegistry& registry) {
        using namespace kernel::obj;
        return registry.register_type(TYPE_ID, "thread", RIGHT_READ | RIGHT_WAIT | RIGHT_DUPLICATE,
                                      RIGHT_READ | RIGHT_WAIT);
    }

   private:
    // Mutated only with interrupts disabled on the scheduling core; no atomics until SMP scheduling.
    thread_state m_state     = thread_state::READY;
    uintptr_t m_kstack_phys  = 0;
    uintptr_t m_kstack_floor = 0;
    uintptr_t m_saved_sp     = 0;
    uint32_t m_slice         = CONFIG_SCHED_TIMESLICE_TICKS;
};

}  // namespace kernel::sched
