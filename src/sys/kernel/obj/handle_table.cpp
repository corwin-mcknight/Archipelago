#include <kernel/obj/handle_table.h>
#include <kernel/obj/type_registry.h>
#include <kernel/panic.h>

namespace kernel::obj {

// Keep the ABI error-sign bit clear; retire saturated slots to prevent stale handles revalidating.
constexpr uint32_t MAX_GENERATION = 0x7FFFFFFF;

HandleTable::~HandleTable()       = default;

ktl::result<void> HandleTable::grow() {
    size_t old_size = m_entries.size();
    if (old_size > static_cast<size_t>(INT32_MAX) - GROW_BATCH) { return ktl::err(ktl::errc::capacity_exhausted); }
    if (!m_entries.reserve(old_size + GROW_BATCH)) { return ktl::err(ktl::errc::oom); }
    for (size_t i = 0; i < GROW_BATCH; i++) {
        if (!m_entries.push_back(HandleEntry{})) { panic("handle slot insertion failed despite reservation"); }
    }
    // Allocate in ascending order: the ABI bootstrap handle must occupy slot 0 in a fresh table.
    for (size_t i = old_size + GROW_BATCH; i-- > old_size;) {
        m_entries[i].next_free = m_free_head;
        m_free_head            = static_cast<int32_t>(i);
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

    auto descriptor = g_type_registry.lookup(object->type_id());
    if (!descriptor.has_value()) { return ktl::err(ktl::errc::wrong_type); }
    if ((rights & ~descriptor.value().valid_rights) != 0) { return ktl::err(ktl::errc::rights_violation); }

    kernel::synchronization::lock_guard guard(m_lock);

    if (m_free_head == -1) {
        auto grown = grow();
        if (grown.is_err()) { return ktl::err(grown.unwrap_err()); }
    }

    if (m_free_head < 0) { panic("handle growth produced no free slot"); }
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

ktl::result<HandleId> HandleTable::insert(ktl::ref<Object> object, Rights rights) {
    return create_handle(ktl::move(object), rights);
}

void HandleTable::clear() {
    // Destructors may re-enter this table, so release each reference after unlocking.
    size_t index = 0;
    for (;;) {
        ktl::ref<Object> victim;  // declared before the guard so it drops after the unlock
        kernel::synchronization::lock_guard guard(m_lock);
        while (index < m_entries.size() && !m_entries[index].object) { index++; }
        if (index >= m_entries.size()) { break; }
        auto& entry  = m_entries[index];
        victim       = ktl::move(entry.object);
        entry.rights = 0;
        m_count--;
        if (entry.generation != MAX_GENERATION) { entry.generation++; }
    }

    // Restore ascending allocation order, excluding retired slots.
    kernel::synchronization::lock_guard guard(m_lock);
    m_free_head = -1;
    for (size_t i = m_entries.size(); i-- > 0;) {
        auto& entry = m_entries[i];
        if (entry.object || entry.generation == MAX_GENERATION) {
            entry.next_free = -1;
            continue;
        }
        entry.next_free = m_free_head;
        m_free_head     = static_cast<int32_t>(i);
    }
}

ktl::result<HandleId> HandleTable::duplicate(HandleId source, Rights rights_mask) {
    ktl::ref<Object> obj_copy;
    Rights new_rights;

    {
        kernel::synchronization::lock_guard guard(m_lock);
        HandleEntry* src = lookup_entry(source);
        if (!src) { return ktl::err(ktl::errc::handle_invalid); }
        if ((src->rights & RIGHT_DUPLICATE) == 0) { return ktl::err(ktl::errc::rights_violation); }
        new_rights = src->rights & rights_mask;
        obj_copy   = src->object;
    }

    return create_handle(ktl::move(obj_copy), new_rights);
}

ktl::result<void> HandleTable::restrict_rights(HandleId id, Rights rights) {
    kernel::synchronization::lock_guard guard(m_lock);
    HandleEntry* entry = lookup_entry(id);
    if (!entry) { return ktl::err(ktl::errc::handle_invalid); }
    if ((rights & ~entry->rights) != 0) { return ktl::err(ktl::errc::rights_violation); }
    entry->rights = rights;
    return ktl::result<void>::ok();
}

ktl::result<void> HandleTable::remove_rights(HandleId id, Rights rights) {
    kernel::synchronization::lock_guard guard(m_lock);
    HandleEntry* entry = lookup_entry(id);
    if (!entry) { return ktl::err(ktl::errc::handle_invalid); }
    entry->rights &= ~rights;
    return ktl::result<void>::ok();
}

ktl::result<void> HandleTable::close(HandleId id) {
    auto taken = take(id);
    if (taken.is_err()) { return ktl::err(taken.unwrap_err()); }
    // Drop outside take()'s lock: destructors may re-enter this table.
    return ktl::result<void>::ok();
}

ktl::result<VerifiedHandle> HandleTable::take(HandleId id) {
    kernel::synchronization::lock_guard guard(m_lock);

    HandleEntry* entry = lookup_entry(id);
    if (!entry) { return ktl::err(ktl::errc::handle_invalid); }

    ktl::ref<Object> moved = ktl::move(entry->object);
    Rights rights          = entry->rights;
    entry->rights          = 0;
    m_count--;

    if (entry->generation == MAX_GENERATION) {
        entry->next_free = -1;
    } else {
        entry->generation++;
        entry->next_free = m_free_head;
        m_free_head      = static_cast<int32_t>(id.index);
    }

    return ktl::result<VerifiedHandle>::ok(VerifiedHandle{ktl::move(moved), rights});
}

ktl::result<VerifiedHandle> HandleTable::verify(HandleId id, Rights required_rights, TypeId expected_type) {
    kernel::synchronization::lock_guard guard(m_lock);
    HandleEntry* entry = lookup_entry(id);
    if (!entry) { return ktl::err(ktl::errc::handle_invalid); }
    if (expected_type != type_ids::INVALID && entry->object->type_id() != expected_type) {
        return ktl::err(ktl::errc::wrong_type);
    }
    if ((entry->rights & required_rights) != required_rights) { return ktl::err(ktl::errc::rights_violation); }
    return ktl::result<VerifiedHandle>::ok(VerifiedHandle{entry->object, entry->rights});
}

size_t HandleTable::count() {
    kernel::synchronization::lock_guard guard(m_lock);
    return m_count;
}

bool HandleTable::snapshot(ktl::vector<HandleInfo>& out) {
    kernel::synchronization::lock_guard guard(m_lock);
    if (m_count > SIZE_MAX - out.size()) { return false; }
    if (!out.reserve(out.size() + m_count)) { return false; }
    for (size_t i = 0; i < m_entries.size(); i++) {
        auto& entry = m_entries[i];
        if (!entry.object) { continue; }
        HandleInfo info;
        info.id        = HandleId{static_cast<uint32_t>(i), entry.generation};
        info.rights    = entry.rights;
        info.type_id   = entry.object->type_id();
        info.object_id = entry.object->id();
        if (!out.push_back(info)) { return false; }
    }
    return true;
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
