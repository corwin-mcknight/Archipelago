#include <kernel/obj/handle_table.h>

namespace kernel::obj {

HandleTable::~HandleTable() {
    for (size_t i = 0; i < m_entries.size(); i++) {
        if (m_entries[i].object) { m_entries[i].object.reset(); }
    }
}

Result<bool, result_t> HandleTable::grow() {
    size_t old_size = m_entries.size();
    for (size_t i = 0; i < GROW_BATCH; i++) {
        HandleEntry entry;
        entry.next_free = m_free_head;
        if (!m_entries.push_back(ktl::move(entry))) { return Result<bool, result_t>::err(RESULT_OOM); }
        m_free_head = static_cast<int32_t>(old_size + i);
    }
    return Result<bool, result_t>::ok(true);
}

HandleTable::HandleEntry* HandleTable::lookup_entry(HandleId id) {
    if (id.index >= m_entries.size()) { return nullptr; }
    auto& entry = m_entries[id.index];
    if (entry.generation != id.generation) { return nullptr; }
    if (!entry.object) { return nullptr; }
    return &entry;
}

Result<HandleId, result_t> HandleTable::create_handle(ktl::ref<Object> object, Rights rights) {
    m_lock.lock();

    if (m_free_head == -1) {
        auto grow_result = grow();
        if (grow_result.is_err()) {
            m_lock.unlock();
            return Result<HandleId, result_t>::err(grow_result.unwrap_err());
        }
    }

    int32_t slot    = m_free_head;
    auto& entry     = m_entries[static_cast<size_t>(slot)];
    m_free_head     = entry.next_free;

    entry.object    = ktl::move(object);
    entry.rights    = rights;
    entry.next_free = -1;
    m_count++;

    HandleId id{static_cast<uint32_t>(slot), entry.generation};

    m_lock.unlock();
    return Result<HandleId, result_t>::ok(id);
}

Result<HandleId, result_t> HandleTable::duplicate(HandleId source, Rights rights_mask) {
    m_lock.lock();

    HandleEntry* src = lookup_entry(source);
    if (!src) {
        m_lock.unlock();
        return Result<HandleId, result_t>::err(RESULT_HANDLE_INVALID);
    }

    Rights new_rights         = src->rights & rights_mask;
    ktl::ref<Object> obj_copy = src->object;

    m_lock.unlock();
    return create_handle(ktl::move(obj_copy), new_rights);
}

Result<bool, result_t> HandleTable::close(HandleId id) {
    m_lock.lock();

    HandleEntry* entry = lookup_entry(id);
    if (!entry) {
        m_lock.unlock();
        return Result<bool, result_t>::err(RESULT_HANDLE_INVALID);
    }

    entry->object.reset();
    entry->rights = 0;
    entry->generation++;
    entry->next_free = m_free_head;
    m_free_head      = static_cast<int32_t>(id.index);
    m_count--;

    m_lock.unlock();
    return Result<bool, result_t>::ok(true);
}

ktl::maybe<HandleInfo> HandleTable::info(HandleId id) {
    m_lock.lock();

    HandleEntry* entry = lookup_entry(id);
    if (!entry) {
        m_lock.unlock();
        return ktl::nothing;
    }

    HandleInfo result;
    result.id        = id;
    result.rights    = entry->rights;
    result.type_id   = entry->object->type_id();
    result.object_id = entry->object->id();

    m_lock.unlock();
    return result;
}

bool HandleTable::is_valid(HandleId id) {
    m_lock.lock();
    HandleEntry* entry = lookup_entry(id);
    m_lock.unlock();
    return entry != nullptr;
}

}  // namespace kernel::obj
