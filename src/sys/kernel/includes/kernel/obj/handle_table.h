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

// The packed user-facing form specified in <abi/syscall.h>: slot index in the low 32 bits, slot
// generation in the high 32.
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

// What verify() hands back: the object reference plus the rights the handle actually carries, so
// callers that report or propagate rights need no second lookup.
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

    // Share an already-existing object into this table.
    ktl::result<HandleId> insert(ktl::ref<Object> object, Rights rights);
    // Insert without taking a reference: the entry points at the object but does not keep it
    // alive. Only for self-handles, where the object outlives the entry by construction (the
    // entry lives inside the object's own table).
    ktl::result<HandleId> insert_unowned(const ktl::ref<Object>& object, Rights rights);
    // Close every live handle and rebuild the free list.
    void clear();

    ktl::result<HandleId> duplicate(HandleId source, Rights rights_mask);
    ktl::result<void> close(HandleId id);
    // Remove a live entry but hand its reference and rights to the caller instead of dropping
    // them. This is the move half of handle transfer: the returned reference is what keeps the
    // object alive while the handle is between tables.
    ktl::result<VerifiedHandle> take(HandleId id);

    // The one verification path every handle operation goes through: slot-and-generation lookup,
    // then type check, then rights check, under a single lock acquisition. Errors come out in that
    // order (handle_invalid, wrong_type, rights_violation) so a caller learns the first thing wrong
    // and nothing more. expected_type type_ids::INVALID means any type.
    ktl::result<VerifiedHandle> verify(HandleId id, Rights required_rights, TypeId expected_type = type_ids::INVALID);

    template <typename T> ktl::result<ktl::ref<T>> get(HandleId id, Rights required_rights = 0);

    ktl::maybe<HandleInfo> info(HandleId id);
    bool is_valid(HandleId id);
    size_t count();
    // Copy every live entry's metadata under one lock acquisition, so callers can print or
    // inspect without holding it. Returns false if the copy failed to allocate; out may be
    // partially filled in that case.
    bool snapshot(ktl::vector<HandleInfo>& out);

#if CONFIG_KERNEL_TESTING
    // Test-only seam: force a slot's generation so generation-wrap retirement can be
    // exercised without 2^32 close/reopen cycles. Returns the updated id for the live handle.
    ktl::maybe<HandleId> testing_set_generation(HandleId id, uint32_t generation);
#endif

   private:
    struct HandleEntry {
        // `object` marks a live entry and is what lookups read; `strong` holds the table's
        // reference and stays empty for unowned (self-handle) entries.
        ktl::unowned_ref<Object> object;
        ktl::ref<Object> strong;
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
    ktl::result<HandleId> create_handle(ktl::ref<Object> object, Rights rights, bool owning);
};

// Template implementations

template <typename T, typename... Args> ktl::result<HandleId> HandleTable::emplace(Rights rights, Args&&... args) {
    auto obj = ktl::make_ref<T>(ktl::forward<Args>(args)...);
    if (!obj) { return ktl::err(ktl::errc::oom); }
    ktl::ref<Object> base_ref = obj;
    return create_handle(ktl::move(base_ref), rights, true);
}

template <typename T> ktl::result<ktl::ref<T>> HandleTable::get(HandleId id, Rights required_rights) {
    auto verified = verify(id, required_rights, T::TYPE_ID);
    if (verified.is_err()) { return ktl::err(verified.unwrap_err()); }
    return ktl::result<ktl::ref<T>>::ok(ktl::static_ref_cast<T>(verified.unwrap().object));
}

}  // namespace kernel::obj
