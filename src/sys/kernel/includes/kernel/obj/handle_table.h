#pragma once

#include <kernel/config.h>
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

    template <typename T, typename... Args> ktl::result<HandleId> emplace(Rights rights, Args&&... args);

    // Share an already-existing object into this table.
    ktl::result<HandleId> insert(ktl::ref<Object> object, Rights rights);
    // Close every live handle and rebuild the free list.
    void clear();

    ktl::result<HandleId> duplicate(HandleId source, Rights rights_mask);
    ktl::result<void> close(HandleId id);

    template <typename T> ktl::result<ktl::ref<T>> get(HandleId id, Rights required_rights = 0);

    ktl::maybe<HandleInfo> info(HandleId id);
    bool is_valid(HandleId id);
    size_t count();

#if CONFIG_KERNEL_TESTING
    // Test-only seam: force a slot's generation so generation-wrap retirement (F021) can be
    // exercised without 2^32 close/reopen cycles. Returns the updated id for the live handle.
    ktl::maybe<HandleId> testing_set_generation(HandleId id, uint32_t generation);
#endif

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

    ktl::result<void> grow();
    HandleEntry* lookup_entry(HandleId id);
    ktl::result<HandleId> create_handle(ktl::ref<Object> object, Rights rights);
};

// Template implementations

template <typename T, typename... Args> ktl::result<HandleId> HandleTable::emplace(Rights rights, Args&&... args) {
    auto obj = ktl::make_ref<T>(ktl::forward<Args>(args)...);
    if (!obj) { return ktl::err(ktl::errc::oom); }
    ktl::ref<Object> base_ref = obj;
    return create_handle(ktl::move(base_ref), rights);
}

template <typename T> ktl::result<ktl::ref<T>> HandleTable::get(HandleId id, Rights required_rights) {
    kernel::synchronization::lock_guard guard(m_lock);
    HandleEntry* entry = lookup_entry(id);
    if (!entry) { return ktl::err(ktl::errc::handle_invalid); }
    if (entry->object->type_id() != T::TYPE_ID) { return ktl::err(ktl::errc::wrong_type); }
    if ((entry->rights & required_rights) != required_rights) { return ktl::err(ktl::errc::rights_violation); }
    return ktl::result<ktl::ref<T>>::ok(ktl::static_ref_cast<T>(entry->object));
}

}  // namespace kernel::obj
