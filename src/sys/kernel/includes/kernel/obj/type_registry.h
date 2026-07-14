#pragma once

#include <kernel/config.h>
#include <kernel/obj/type_descriptor.h>
#include <kernel/obj/types.h>
#include <kernel/synchronization/mutex.h>

#include <ktl/atomic>
#include <ktl/maybe>
#include <ktl/result>

namespace kernel::obj {

class TypeRegistry {
   public:
    ktl::result<void> register_type(TypeId id, ktl::string_view name, Rights valid_rights, Rights default_rights);
    ktl::maybe<const TypeDescriptor&> lookup(TypeId id) const;
    ktl::maybe<const TypeDescriptor&> lookup_by_name(ktl::string_view name) const;
    size_t count() const;

    void on_object_created(TypeId id);
    void on_object_destroyed(TypeId id);
    uint32_t live_count(TypeId id) const;

   private:
    static constexpr size_t MAX_TYPES                  = CONFIG_MAX_OBJECT_TYPES;
    TypeDescriptor m_types[MAX_TYPES]                  = {};
    ktl::atomic<uint32_t> m_instance_counts[MAX_TYPES] = {};
    size_t m_count                                     = 0;
    kernel::synchronization::mutex m_lock;

    ktl::maybe<size_t> index_for_id(TypeId id) const;
};

extern TypeRegistry g_type_registry;

}  // namespace kernel::obj
