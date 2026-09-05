#pragma once

#include <kernel/config.h>
#include <kernel/obj/object.h>
#include <kernel/obj/types.h>
#include <kernel/synchronization/mutex.h>

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

// ABI encoding: index in the low 32 bits, generation in the high 32.
inline uint64_t pack_handle(HandleId id) {
    return static_cast<uint64_t>(id.index) | (static_cast<uint64_t>(id.generation) << 32);
}

inline HandleId unpack_handle(uint64_t handle) {
    return HandleId{static_cast<uint32_t>(handle), static_cast<uint32_t>(handle >> 32)};
}

struct HandleInfo {
    HandleId id;
    Rights rights;
    TypeId type_id;
    ObjectId object_id;
};

struct VerifiedHandle {
    ktl::ref<Object> object;
    Rights rights;
};

class HandleTable {
   public:
    HandleTable() = default;
    ~HandleTable();

    HandleTable(const HandleTable&)            = delete;
    HandleTable& operator=(const HandleTable&) = delete;

    template <typename T, typename... Args> ktl::result<HandleId> emplace(Rights rights, Args&&... args);

    ktl::result<HandleId> insert(ktl::ref<Object> object, Rights rights);
    void clear();

    ktl::result<HandleId> duplicate(HandleId source, Rights rights_mask);
    // Exact subset, checked and applied under one lock. No allocation or reference changes.
    ktl::result<void> restrict_rights(HandleId id, Rights rights);
    // Clear these bits from the current rights under the lock; only invalid handles fail.
    ktl::result<void> remove_rights(HandleId id, Rights rights);
    ktl::result<void> close(HandleId id);
    // Remove the handle and transfer its ownership to the caller.
    ktl::result<VerifiedHandle> take(HandleId id);

    // Under one lock, check validity, type, then rights; errors follow that order.
    // type_ids::INVALID accepts any type. The returned reference pins the object after unlock.
    ktl::result<VerifiedHandle> verify(HandleId id, Rights required_rights, TypeId expected_type = type_ids::INVALID);

    template <typename T> ktl::result<ktl::ref<T>> get(HandleId id, Rights required_rights = 0);

    bool is_valid(HandleId id);
    size_t count();
    // Append live metadata under one lock. Allocation failure returns false and may leave a partial copy.
    bool snapshot(ktl::vector<HandleInfo>& out);

#if CONFIG_KERNEL_TESTING
    // Force generation retirement in tests; returns the updated live handle ID.
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
    kernel::synchronization::mutex m_lock;

    static constexpr size_t GROW_BATCH = 32;

    ktl::result<void> grow();
    HandleEntry* lookup_entry(HandleId id);
    ktl::result<HandleId> create_handle(ktl::ref<Object> object, Rights rights);
};

template <typename T, typename... Args> ktl::result<HandleId> HandleTable::emplace(Rights rights, Args&&... args) {
    auto obj = ktl::make_ref<T>(ktl::forward<Args>(args)...);
    if (!obj) { return ktl::err(ktl::errc::oom); }
    ktl::ref<Object> base_ref = obj;
    return create_handle(ktl::move(base_ref), rights);
}

template <typename T> ktl::result<ktl::ref<T>> HandleTable::get(HandleId id, Rights required_rights) {
    auto verified = verify(id, required_rights, T::TYPE_ID);
    if (verified.is_err()) { return ktl::err(verified.unwrap_err()); }
    return ktl::result<ktl::ref<T>>::ok(ktl::static_ref_cast<T>(verified.unwrap().object));
}

}  // namespace kernel::obj
