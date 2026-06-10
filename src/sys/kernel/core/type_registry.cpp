#include <kernel/obj/type_registry.h>

#include <ktl/algorithm>
#include <ktl/string_view>

namespace kernel::obj {

TypeRegistry g_type_registry;

using TypeOrResult = Result<TypeId, result_t>;

TypeOrResult TypeRegistry::register_type(TypeId id, ktl::string_view name, Rights valid_rights, Rights default_rights) {
    kernel::synchronization::lock_guard guard(m_lock);

    if (m_count >= MAX_TYPES) { return TypeOrResult::err(RESULT_REGISTRY_FULL); }

    auto duplicate =
        ktl::find_if(m_types, m_types + m_count, [&](const TypeDescriptor& t) { return t.id == id || t.name == name; });
    if (duplicate.has_value()) { return TypeOrResult::err(RESULT_ALREADY_REGISTERED); }

    m_types[m_count] = {id, name, valid_rights, default_rights};
    m_count++;

    return TypeOrResult::ok(id);
}

size_t TypeRegistry::count() const { return m_count; }

ktl::maybe<const TypeDescriptor&> TypeRegistry::lookup(TypeId id) const {
    return ktl::find_if(m_types, m_types + m_count, [&](const TypeDescriptor& t) { return t.id == id; });
}

ktl::maybe<const TypeDescriptor&> TypeRegistry::lookup_by_name(ktl::string_view name) const {
    return ktl::find_if(m_types, m_types + m_count, [&](const TypeDescriptor& t) { return t.name == name; });
}

void TypeRegistry::on_object_created(TypeId id) {
    index_for_id(id).inspect([&](size_t idx) { m_instance_counts[idx].fetch_add(1, ktl::memory_order::relaxed); });
}

void TypeRegistry::on_object_destroyed(TypeId id) {
    index_for_id(id).inspect([&](size_t idx) { m_instance_counts[idx].fetch_sub(1, ktl::memory_order::relaxed); });
}

uint32_t TypeRegistry::live_count(TypeId id) const {
    return index_for_id(id).map_or([&](size_t idx) { return m_instance_counts[idx].load(ktl::memory_order::relaxed); },
                                   0u);
}

ktl::maybe<size_t> TypeRegistry::index_for_id(TypeId id) const {
    return ktl::find_index_if(m_types, m_types + m_count, [&](const TypeDescriptor& t) { return t.id == id; });
}

}  // namespace kernel::obj
