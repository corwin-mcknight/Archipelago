#pragma once

#include <kernel/obj/channel.h>
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

    // The parent's end of this task's bootstrap channel (<abi/syscall.h>). The kernel is every
    // task's parent today, so the end lives here rather than in a handle table; writing to it
    // queues mail on the task's slot-0 endpoint. Held for the task's life -- dropping it is what
    // tells the task its parent is gone -- and released in teardown_user_task.
    const ktl::ref<kernel::obj::Channel>& mailbox() const { return m_mailbox; }
    void set_mailbox(ktl::ref<kernel::obj::Channel> end) { m_mailbox = ktl::move(end); }

    // IPC-buffer slots in this task's address space. Threads need distinct buffer addresses and
    // the VMM has no first-fit search, so slots are handed out from a bitmap and returned when the
    // thread goes away -- a monotonic counter would leak the address space of a task that spawns
    // and reaps workers over a long life.
    //
    // Bookkeeping only: a slot is not reusable until its buffer is also unmapped, which is VMM work
    // this class deliberately does not do. Use kernel::sched::release_thread_ipc() to do both.
    ktl::maybe<size_t> acquire_ipc_slot();
    void release_ipc_slot(size_t slot);

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
    ktl::ref<kernel::obj::Channel> m_mailbox;
    // One bit per slot; see IPC_BUFFER_MAX_SLOTS.
    uint64_t m_ipc_slots = 0;
};

// Task zero. Lazy-created on first use: kernel boot reaches it from obj_init(), host tests
// directly. The kernel's own threads and handles live here; there is no global handle table.
ktl::ref<Task> kernel_task();

void register_task(ktl::ref<Task> task);
void unregister_task(kernel::obj::ObjectId task_id);
bool snapshot_tasks(ktl::vector<ktl::ref<Task>>& out);

}  // namespace kernel::sched
