#pragma once

#include <kernel/mm/vm_aspace.h>
#include <kernel/mm/vmo.h>
#include <kernel/obj/object.h>
#include <kernel/obj/type_registry.h>

#include <ktl/result>

namespace kernel::sched {

bool valid_user_start(mm::vm_aspace& space, uintptr_t entry, uintptr_t stack);

// Minted at boot, explicitly endowed to loaders. A normal Task handle does not carry this
// authority, and starting a child never implicitly copies it into that child's handle table.
class TaskFactory : public obj::Object {
   public:
    DECLARE_OBJECT_TYPE(TaskFactory, obj::type_ids::TASK_FACTORY)
    TaskFactory() : Object(TYPE_ID) {}
    static ktl::result<void> register_type(obj::TypeRegistry& registry) {
        return registry.register_type(TYPE_ID, "task_factory", obj::RIGHT_WRITE | obj::RIGHT_DUPLICATE,
                                      obj::RIGHT_WRITE);
    }
};

// Bound to one task by identity, without retaining it through its own handle table.
class ThreadFactory : public obj::Object {
   public:
    DECLARE_OBJECT_TYPE(ThreadFactory, obj::type_ids::THREAD_FACTORY)
    explicit ThreadFactory(obj::ObjectId owner) : Object(TYPE_ID), m_owner(owner) {}
    bool permits(obj::ObjectId caller) const { return caller == m_owner; }
    static ktl::result<void> register_type(obj::TypeRegistry& registry) {
        return registry.register_type(TYPE_ID, "thread_factory", obj::RIGHT_WRITE | obj::RIGHT_DUPLICATE,
                                      obj::RIGHT_WRITE);
    }

   private:
    obj::ObjectId m_owner;
};

// One private construction per loading thread. No handle to this container is exposed: other
// threads cannot modify it, race its start, or keep it alive by transferring a handle elsewhere.
// The reaper releases it when its owning loading thread exits, even if that thread's task lives.
class TaskConstruction {
   public:
    static constexpr size_t NAME_CAPACITY = 64;
    static constexpr uint64_t MAX_BYTES   = 64ull * 1024 * 1024;
    TaskConstruction()                    = default;
    ~TaskConstruction();
    TaskConstruction(const TaskConstruction&)            = delete;
    TaskConstruction& operator=(const TaskConstruction&) = delete;

    ktl::result<void> init(const char* name, size_t length);
    // Snapshot page-aligned source bytes into private backing. The loader has no handle or
    // mapping to that backing, so RX can never alias a writable source mapping.
    ktl::result<void> map(mm::vmo& source, uint64_t offset, uintptr_t address, size_t size, mm::vm_prot_t prot);
    bool valid_start(uintptr_t entry, uintptr_t stack) const;
    mm::vm_aspace* release();
    const char* name() const { return m_name; }

   private:
    mm::vm_aspace* m_space     = nullptr;
    uint64_t m_bytes           = 0;
    char m_name[NAME_CAPACITY] = {};
};

}  // namespace kernel::sched
