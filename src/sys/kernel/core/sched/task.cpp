#include <kernel/sched/task.h>

namespace kernel::sched {

namespace { ktl::ref<Task> g_kernel_task; }  // namespace

ktl::ref<Task> kernel_task() {
    // First call happens single-threaded (obj_init at boot; fork-isolated host tests).
    if (!g_kernel_task) {
        g_kernel_task = ktl::make_ref<Task>();
        if (g_kernel_task) { g_kernel_task->set_name("kernel"); }
    }
    return g_kernel_task;
}

ktl::result<void> Task::add_thread(ktl::ref<Thread> thread) {
    kernel::synchronization::lock_guard guard(m_lock);
    if (!m_threads.push_back(ktl::move(thread))) { return ktl::err(ktl::errc::oom); }
    return ktl::result<void>::ok();
}

void Task::remove_thread(kernel::obj::ObjectId thread_id) {
    kernel::synchronization::lock_guard guard(m_lock);
    for (size_t i = 0; i < m_threads.size(); ++i) {
        if (m_threads[i]->id() == thread_id) {
            m_threads[i] = ktl::move(m_threads[m_threads.size() - 1]);
            auto discard = m_threads.pop_back();
            return;
        }
    }
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

}  // namespace kernel::sched
