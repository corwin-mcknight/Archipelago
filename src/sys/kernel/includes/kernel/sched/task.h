#pragma once

#include <kernel/obj/handle_table.h>
#include <kernel/obj/object.h>
#include <kernel/obj/type_registry.h>
#include <kernel/obj/types.h>
#include <kernel/sched/thread.h>
#include <kernel/synchronization/spinlock.h>

#include <ktl/ref>
#include <ktl/result>
#include <ktl/vector>

namespace kernel::sched {

class Task : public kernel::obj::Object {
   public:
    DECLARE_OBJECT_TYPE(Task, kernel::obj::type_ids::TASK)

    Task() : Object(TYPE_ID) {}

    kernel::obj::HandleTable& handles() { return m_handles; }

    ktl::result<void> add_thread(ktl::ref<Thread> thread);
    void remove_thread(kernel::obj::ObjectId thread_id);
    size_t thread_count();

    // Copies refs to all threads under the task lock, so callers can inspect or format
    // without holding it. Returns false if the copy failed to allocate.
    bool snapshot_threads(ktl::vector<ktl::ref<Thread>>& out);

    static ktl::result<void> register_type(kernel::obj::TypeRegistry& registry) {
        using namespace kernel::obj;
        return registry.register_type(TYPE_ID, "task", RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE, RIGHT_READ);
    }

   private:
    kernel::obj::HandleTable m_handles;
    ktl::vector<ktl::ref<Thread>> m_threads;
    kernel::synchronization::spinlock m_lock;
};

// Task zero. Lazy-created on first use: kernel boot reaches it from obj_init(), host tests
// directly. The kernel's own threads and handles live here; there is no global handle table.
ktl::ref<Task> kernel_task();

}  // namespace kernel::sched
