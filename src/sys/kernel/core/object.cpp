#include <kernel/obj/counter.h>
#include <kernel/obj/event.h>
#include <kernel/obj/object.h>
#include <kernel/obj/semaphore.h>
#include <kernel/obj/type_registry.h>
#include <kernel/sched/task.h>
#include <kernel/sched/thread.h>

namespace kernel::obj {

// VMM object types (Region, VMO) register in vmm_init -- mm code stays out of
// the host-tier build.
void obj_init() {
    Event::register_type(g_type_registry).expect("obj_init: Event type registration failed");
    Counter::register_type(g_type_registry).expect("obj_init: Counter type registration failed");
    Semaphore::register_type(g_type_registry).expect("obj_init: Semaphore type registration failed");
    kernel::sched::Thread::register_type(g_type_registry).expect("obj_init: Thread type registration failed");
    kernel::sched::Task::register_type(g_type_registry).expect("obj_init: Task type registration failed");
    kernel::sched::kernel_task();  // task zero exists before anything can need a handle
}

namespace { ktl::atomic<uint64_t> g_next_object_id{1}; }  // namespace

Object::Object(TypeId type_id) : m_id(allocate_id()), m_type_id(type_id) {
    g_type_registry.on_object_created(m_type_id);
}

ObjectId Object::allocate_id() { return g_next_object_id.fetch_add(1, ktl::memory_order::relaxed); }
Object::~Object() { g_type_registry.on_object_destroyed(m_type_id); }
uint32_t Object::signals() const { return m_signals.load(ktl::memory_order::acquire); }
void Object::signal_set(uint32_t bits) {
    m_signals.fetch_or(bits, ktl::memory_order::seq_cst);
    object_signal_wake(this);
}
void Object::signal_clear(uint32_t bits) { m_signals.fetch_and(~bits, ktl::memory_order::seq_cst); }

}  // namespace kernel::obj
