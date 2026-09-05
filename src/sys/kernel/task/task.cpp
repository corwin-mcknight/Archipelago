#include <kernel/sched/task.h>

namespace kernel::sched {

namespace {
[[clang::no_destroy]] ktl::ref<Task> g_kernel_task;
[[clang::no_destroy]] ktl::vector<ktl::ref<Task>> g_tasks;
[[clang::no_destroy]] kernel::synchronization::mutex g_tasks_lock;
}  // namespace

ktl::ref<Task> kernel_task() {
    // First call happens single-threaded (obj_init at boot; fork-isolated host tests).
    if (!g_kernel_task) {
        g_kernel_task = ktl::make_ref<Task>();
        if (g_kernel_task) {
            g_kernel_task->set_name("kernel");
            g_kernel_task->set_state(task_state::RUNNING);
            register_task(g_kernel_task);
        }
    }
    return g_kernel_task;
}

task_state Task::state() {
    kernel::synchronization::lock_guard guard(m_lock);
    return m_state;
}

void Task::set_state(task_state state) {
    kernel::synchronization::lock_guard guard(m_lock);
    m_state = state;
}

ktl::result<void> Task::add_thread(ktl::ref<Thread> thread) {
    if (!thread) { return ktl::err(ktl::errc::null_argument); }
    if (thread->owner().get() != this) { return ktl::err(ktl::errc::invalid_operation); }
    kernel::synchronization::lock_guard guard(m_lock);
    if (!m_threads.push_back(ktl::move(thread))) { return ktl::err(ktl::errc::oom); }
    return ktl::result<void>::ok();
}

void Task::remove_thread(kernel::obj::ObjectId thread_id) {
    // The thread's IPC slot is NOT reclaimed here. Freeing it means unmapping the buffer, which is
    // VMM work, and this file is linked by the host test tier as pure bookkeeping. Callers pair
    // this with release_thread_ipc() -- see reaper.cpp and spawn.cpp.
    kernel::synchronization::lock_guard guard(m_lock);
    for (size_t i = 0; i < m_threads.size(); ++i) {
        if (m_threads[i]->id() == thread_id) {
            m_threads.swap_remove(i);
            return;
        }
    }
}

ktl::maybe<size_t> Task::acquire_ipc_slot() {
    kernel::synchronization::lock_guard guard(m_lock);
    for (size_t slot = 0; slot < IPC_BUFFER_MAX_SLOTS; ++slot) {
        uint64_t bit = uint64_t{1} << slot;
        if ((m_ipc_slots & bit) == 0) {
            m_ipc_slots |= bit;
            return slot;
        }
    }
    return ktl::nothing;
}

void Task::release_ipc_slot(size_t slot) {
    kernel::synchronization::lock_guard guard(m_lock);
    m_ipc_slots &= ~(uint64_t{1} << slot);
}

size_t Task::thread_count() {
    kernel::synchronization::lock_guard guard(m_lock);
    return m_threads.size();
}

bool Task::snapshot_threads(ktl::vector<ktl::ref<Thread>>& out) {
    kernel::synchronization::lock_guard guard(m_lock);
    for (size_t i = 0; i < m_threads.size(); ++i) {
        if (!out.push_back(m_threads[i])) { return false; }
    }
    return true;
}

void register_task(ktl::ref<Task> task) {
    kernel::synchronization::lock_guard guard(g_tasks_lock);
    (void)g_tasks.push_back(ktl::move(task));
}

void unregister_task(kernel::obj::ObjectId task_id) {
    kernel::synchronization::lock_guard guard(g_tasks_lock);
    for (size_t i = 0; i < g_tasks.size(); ++i) {
        if (g_tasks[i]->id() == task_id) {
            g_tasks.swap_remove(i);
            return;
        }
    }
}

bool snapshot_tasks(ktl::vector<ktl::ref<Task>>& out) {
    kernel::synchronization::lock_guard guard(g_tasks_lock);
    for (size_t i = 0; i < g_tasks.size(); ++i) {
        if (!out.push_back(g_tasks[i])) { return false; }
    }
    return true;
}

}  // namespace kernel::sched
