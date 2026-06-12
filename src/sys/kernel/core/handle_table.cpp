#include <kernel/obj/handle_table.h>
#include <kernel/obj/type_registry.h>

namespace kernel::obj {

HandleTable g_handle_table;

HandleTable::~HandleTable() = default;

ktl::result<void> HandleTable::grow() {
    size_t old_size = m_entries.size();
    for (size_t i = 0; i < GROW_BATCH; i++) {
        HandleEntry entry;
        entry.next_free = m_free_head;
        if (!m_entries.push_back(ktl::move(entry))) { return ktl::err(ktl::errc::oom); }
        m_free_head = static_cast<int32_t>(old_size + i);
    }
    return ktl::result<void>::ok();
}

HandleTable::HandleEntry* HandleTable::lookup_entry(HandleId id) {
    if (id.index >= m_entries.size()) { return nullptr; }
    auto& entry = m_entries[id.index];
    if (entry.generation != id.generation) { return nullptr; }
    if (!entry.object) { return nullptr; }
    return &entry;
}

ktl::result<HandleId> HandleTable::create_handle(ktl::ref<Object> object, Rights rights) {
    if (!object) { return ktl::err(ktl::errc::null_argument); }

    // F033: requested rights must stay within the contract registered for this object's type.
    // Out-of-contract bits are rejected outright rather than silently clamped.
    auto descriptor = g_type_registry.lookup(object->type_id());
    if (!descriptor.has_value()) { return ktl::err(ktl::errc::wrong_type); }
    if ((rights & ~descriptor.value().valid_rights) != 0) { return ktl::err(ktl::errc::rights_violation); }

    kernel::synchronization::lock_guard guard(m_lock);

    if (m_free_head == -1) { KTRY(grow()); }

    int32_t slot    = m_free_head;
    auto& entry     = m_entries[static_cast<size_t>(slot)];
    m_free_head     = entry.next_free;

    entry.object    = ktl::move(object);
    entry.rights    = rights;
    entry.next_free = -1;
    m_count++;

    HandleId id{static_cast<uint32_t>(slot), entry.generation};
    return ktl::result<HandleId>::ok(id);
}

ktl::result<HandleId> HandleTable::duplicate(HandleId source, Rights rights_mask) {
    ktl::ref<Object> obj_copy;
    Rights new_rights;

    {
        kernel::synchronization::lock_guard guard(m_lock);
        HandleEntry* src = lookup_entry(source);
        if (!src) { return ktl::err(ktl::errc::handle_invalid); }
        new_rights = src->rights & rights_mask;
        obj_copy   = src->object;
    }

    return create_handle(ktl::move(obj_copy), new_rights);
}

ktl::result<void> HandleTable::close(HandleId id) {
    kernel::synchronization::lock_guard guard(m_lock);

    HandleEntry* entry = lookup_entry(id);
    if (!entry) { return ktl::err(ktl::errc::handle_invalid); }

    entry->object.reset();
    entry->rights = 0;
    m_count--;

    // F021: if the generation counter is saturated, incrementing would wrap to 0 and let a stale
    // (index, generation) HandleId revalidate against a recycled slot. Retire the slot permanently
    // instead of returning it to the free list.
    if (entry->generation == UINT32_MAX) {
        entry->next_free = -1;
        return ktl::result<void>::ok();
    }

    entry->generation++;
    entry->next_free = m_free_head;
    m_free_head      = static_cast<int32_t>(id.index);

    return ktl::result<void>::ok();
}

ktl::maybe<HandleInfo> HandleTable::info(HandleId id) {
    kernel::synchronization::lock_guard guard(m_lock);

    HandleEntry* entry = lookup_entry(id);
    if (!entry) { return ktl::nothing; }

    HandleInfo result;
    result.id        = id;
    result.rights    = entry->rights;
    result.type_id   = entry->object->type_id();
    result.object_id = entry->object->id();

    return result;
}

size_t HandleTable::count() {
    kernel::synchronization::lock_guard guard(m_lock);
    return m_count;
}

bool HandleTable::is_valid(HandleId id) {
    kernel::synchronization::lock_guard guard(m_lock);
    return lookup_entry(id) != nullptr;
}

#if CONFIG_KERNEL_TESTING
ktl::maybe<HandleId> HandleTable::testing_set_generation(HandleId id, uint32_t generation) {
    kernel::synchronization::lock_guard guard(m_lock);
    HandleEntry* entry = lookup_entry(id);
    if (!entry) { return ktl::nothing; }
    entry->generation = generation;
    return HandleId{id.index, generation};
}
#endif

}  // namespace kernel::obj
