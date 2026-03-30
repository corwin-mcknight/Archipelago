#include <kernel/obj/object.h>
#include <kernel/obj/type_registry.h>

namespace kernel::obj {

namespace { ktl::atomic<uint64_t> g_next_object_id{1}; }  // namespace

ObjectId Object::allocate_id() { return g_next_object_id.fetch_add(1, ktl::memory_order::relaxed); }

Object::Object(TypeId type_id) : m_id(allocate_id()), m_type_id(type_id) {
    g_type_registry.on_object_created(m_type_id);
}

Object::~Object() { g_type_registry.on_object_destroyed(m_type_id); }

uint32_t Object::signals() const { return m_signals.load(ktl::memory_order::acquire); }

void Object::signal_set(uint32_t bits) { m_signals.fetch_or(bits, ktl::memory_order::seq_cst); }

void Object::signal_clear(uint32_t bits) { m_signals.fetch_and(~bits, ktl::memory_order::seq_cst); }

}  // namespace kernel::obj
