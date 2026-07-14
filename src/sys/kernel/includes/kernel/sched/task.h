#pragma once

#include <kernel/obj/handle_table.h>
#include <kernel/obj/object.h>
#include <kernel/obj/type_registry.h>
#include <kernel/obj/types.h>
#include <kernel/sched/thread.h>
#include <kernel/synchronization/mutex.h>

#include <ktl/ref>
#include <ktl/result>
#include <ktl/vector>

namespace kernel::mm { class vm_aspace; }

namespace kernel::sched {

enum class task_state : uint32_t { NEW = 0, RUNNING, TERMINATED };

class Task : public kernel::obj::Object {
   public:
    DECLARE_OBJECT_TYPE(Task, kernel::obj::type_ids::TASK)

    Task() : Object(TYPE_ID) {}

    kernel::obj::HandleTable& handles() { return m_handles; }

    ktl::result<void> add_thread(ktl::ref<Thread> thread);
    void remove_thread(kernel::obj::ObjectId thread_id);
    size_t thread_count();

    // Copies refs to all threads under the task lock, so callers can inspect or format
    // without holding it. Returns false if the copy failed to allocate; out may be partially
    // filled in that case.
    bool snapshot_threads(ktl::vector<ktl::ref<Thread>>& out);

    task_state state();
    void set_state(task_state state);

    kernel::mm::vm_aspace* aspace() const { return m_aspace; }
    void set_aspace(kernel::mm::vm_aspace* aspace) { m_aspace = aspace; }

    kernel::obj::HandleId owner_handle() const { return m_owner_handle; }
    void set_owner_handle(kernel::obj::HandleId id) { m_owner_handle = id; }

    static ktl::result<void> register_type(kernel::obj::TypeRegistry& registry) {
        using namespace kernel::obj;
        return registry.register_type(TYPE_ID, "task", RIGHT_READ | RIGHT_WRITE | RIGHT_DUPLICATE, RIGHT_READ);
    }

   private:
    kernel::obj::HandleTable m_handles;
    ktl::vector<ktl::ref<Thread>> m_threads;
    kernel::synchronization::mutex m_lock;
    kernel::mm::vm_aspace* m_aspace      = nullptr;
    task_state m_state                   = task_state::NEW;
    kernel::obj::HandleId m_owner_handle = kernel::obj::HandleId::invalid();
};

// Task zero. Lazy-created on first use: kernel boot reaches it from obj_init(), host tests
// directly. The kernel's own threads and handles live here; there is no global handle table.
ktl::ref<Task> kernel_task();

void register_task(ktl::ref<Task> task);
void unregister_task(kernel::obj::ObjectId task_id);
bool snapshot_tasks(ktl::vector<ktl::ref<Task>>& out);

}  // namespace kernel::sched
