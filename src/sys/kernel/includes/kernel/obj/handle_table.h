#pragma once

#include <kernel/obj/object.h>
#include <kernel/obj/types.h>
#include <kernel/synchronization/spinlock.h>

#include <ktl/maybe>
#include <ktl/ref>
#include <ktl/result>
#include <ktl/utility>
#include <ktl/vector>

namespace kernel::obj {

struct HandleId {
    uint32_t index;
    uint32_t generation;

    static HandleId invalid() { return {0xFFFFFFFF, 0}; }
    bool is_valid() const { return index != 0xFFFFFFFF; }
    bool operator==(const HandleId& other) const { return index == other.index && generation == other.generation; }
    bool operator!=(const HandleId& other) const { return !(*this == other); }
};

struct HandleInfo {
    HandleId id;
    Rights rights;
    TypeId type_id;
    ObjectId object_id;
};

class HandleTable {
   public:
    HandleTable() = default;
    ~HandleTable();

    HandleTable(const HandleTable&)            = delete;
    HandleTable& operator=(const HandleTable&) = delete;

    template <typename T, typename... Args> Result<HandleId, result_t> emplace(Rights rights, Args&&... args);

    Result<HandleId, result_t> duplicate(HandleId source, Rights rights_mask);
    Result<bool, result_t> close(HandleId id);

    template <typename T> Result<T*, result_t> get(HandleId id, Rights required_rights = 0);

    ktl::maybe<HandleInfo> info(HandleId id);
    bool is_valid(HandleId id);
    size_t count() const { return m_count; }

   private:
    struct HandleEntry {
        ktl::ref<Object> object;
        Rights rights       = 0;
        uint32_t generation = 0;
        int32_t next_free   = -1;
    };

    ktl::vector<HandleEntry> m_entries;
    size_t m_count      = 0;
    int32_t m_free_head = -1;
    kernel::synchronization::spinlock m_lock;

    static constexpr size_t GROW_BATCH = 32;

    Result<bool, result_t> grow();
    HandleEntry* lookup_entry(HandleId id);
    Result<HandleId, result_t> create_handle(ktl::ref<Object> object, Rights rights);
};

// Template implementations

template <typename T, typename... Args> Result<HandleId, result_t> HandleTable::emplace(Rights rights, Args&&... args) {
    auto obj                  = ktl::make_ref<T>(ktl::forward<Args>(args)...);
    ktl::ref<Object> base_ref = obj;
    return create_handle(ktl::move(base_ref), rights);
}

template <typename T> Result<T*, result_t> HandleTable::get(HandleId id, Rights required_rights) {
    m_lock.lock();
    HandleEntry* entry = lookup_entry(id);
    if (!entry) {
        m_lock.unlock();
        return Result<T*, result_t>::err(RESULT_HANDLE_INVALID);
    }
    if (entry->object->type_id() != T::TYPE_ID) {
        m_lock.unlock();
        return Result<T*, result_t>::err(RESULT_WRONG_TYPE);
    }
    if ((entry->rights & required_rights) != required_rights) {
        m_lock.unlock();
        return Result<T*, result_t>::err(RESULT_RIGHTS_VIOLATION);
    }
    T* ptr = static_cast<T*>(entry->object.get());
    m_lock.unlock();
    return Result<T*, result_t>::ok(ptr);
}

}  // namespace kernel::obj
