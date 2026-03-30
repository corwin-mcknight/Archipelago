#include <kernel/obj/type_registry.h>
#include <std/string.h>

namespace kernel::obj {

namespace {
bool str_equal(const char* a, const char* b) {
    while (*a && *b) {
        if (*a != *b) { return false; }
        a++;
        b++;
    }
    return *a == *b;
}
}  // namespace

TypeRegistry g_type_registry;

Result<TypeId, result_t> TypeRegistry::register_type(TypeId id, const char* name, Rights valid_rights,
                                                     Rights default_rights) {
    kernel::synchronization::lock_guard guard(m_lock);

    if (m_count >= MAX_TYPES) { return Result<TypeId, result_t>::err(RESULT_REGISTRY_FULL); }

    for (size_t i = 0; i < m_count; i++) {
        if (m_types[i].id == id || str_equal(m_types[i].name, name)) {
            return Result<TypeId, result_t>::err(RESULT_ALREADY_REGISTERED);
        }
    }

    m_types[m_count] = {id, name, valid_rights, default_rights};
    m_count++;

    return Result<TypeId, result_t>::ok(id);
}

ktl::maybe<const TypeDescriptor*> TypeRegistry::lookup(TypeId id) const {
    for (size_t i = 0; i < m_count; i++) {
        if (m_types[i].id == id) { return &m_types[i]; }
    }
    return ktl::nothing;
}

ktl::maybe<const TypeDescriptor*> TypeRegistry::lookup_by_name(const char* name) const {
    for (size_t i = 0; i < m_count; i++) {
        if (str_equal(m_types[i].name, name)) { return &m_types[i]; }
    }
    return ktl::nothing;
}

size_t TypeRegistry::count() const { return m_count; }

void TypeRegistry::on_object_created(TypeId id) {
    size_t idx = index_for_id(id);
    if (idx < MAX_TYPES) { m_instance_counts[idx].fetch_add(1, ktl::memory_order::relaxed); }
}

void TypeRegistry::on_object_destroyed(TypeId id) {
    size_t idx = index_for_id(id);
    if (idx < MAX_TYPES) { m_instance_counts[idx].fetch_sub(1, ktl::memory_order::relaxed); }
}

uint32_t TypeRegistry::live_count(TypeId id) const {
    size_t idx = index_for_id(id);
    if (idx >= MAX_TYPES) { return 0; }
    return m_instance_counts[idx].load(ktl::memory_order::relaxed);
}

size_t TypeRegistry::index_for_id(TypeId id) const {
    for (size_t i = 0; i < m_count; i++) {
        if (m_types[i].id == id) { return i; }
    }
    return MAX_TYPES;
}

}  // namespace kernel::obj
