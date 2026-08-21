#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/synchronization/execution_context.h>
#include <kernel/synchronization/lockdep.h>

namespace kernel::synchronization::lockdep {

#ifndef NDEBUG
namespace {
struct lock_record {
    const void* address   = nullptr;
    const char* name      = nullptr;
    size_t owner_cpu      = 0;
    uint64_t owner_thread = 0;
    bool owned            = false;
};
struct edge_record {
    uint32_t from = 0;
    uint32_t to   = 0;
};

lock_record g_locks[CONFIG_LOCKDEP_MAX_LOCKS];
edge_record g_edges[CONFIG_LOCKDEP_MAX_EDGES];
uint32_t g_free_list[CONFIG_LOCKDEP_MAX_LOCKS];
uint32_t g_next_identity      = 1;
size_t g_edge_count           = 0;
size_t g_free_count           = 0;
volatile uint8_t g_table_lock = 0;

// Serializes the tables across cores. A raw spin, since lockdep cannot instrument itself;
// interrupts off so the holder cannot be preempted (or migrated) while inside.
class table_guard {
   public:
    table_guard() : m_flags(kernel::arch::save_and_disable_interrupts()) {
        while (__atomic_exchange_n(&g_table_lock, 1, __ATOMIC_ACQUIRE)) {}
    }
    ~table_guard() {
        __atomic_store_n(&g_table_lock, 0, __ATOMIC_RELEASE);
        kernel::arch::restore_interrupts(m_flags);
    }

   private:
    uint64_t m_flags;
};

lock_record& record(uint32_t identity) {
    if (identity == 0 || identity >= g_next_identity || identity > CONFIG_LOCKDEP_MAX_LOCKS) {
        panic("lockdep: invalid lock identity");
    }
    return g_locks[identity - 1];
}

bool path_exists(uint32_t from, uint32_t to, bool* visited) {
    if (from == to) { return true; }
    if (visited[from - 1]) { return false; }
    visited[from - 1] = true;
    for (size_t i = 0; i < g_edge_count; ++i) {
        if (g_edges[i].from == from && path_exists(g_edges[i].to, to, visited)) { return true; }
    }
    return false;
}

void learn_edge(uint32_t from, uint32_t to) {
    if (from == 0 || to == 0) { return; }
    for (size_t i = 0; i < g_edge_count; ++i) {
        if (g_edges[i].from == from && g_edges[i].to == to) { return; }
    }
    bool visited[CONFIG_LOCKDEP_MAX_LOCKS] = {};
    if (path_exists(to, from, visited)) {
#if defined(ARCH_X86_64) || defined(ARCH_RISCV64)
        g_log.error("lockdep: cycle: {0} at 0x{1:p} held, acquiring {2} at 0x{3:p}", record(from).name,
                    (uintptr_t)record(from).address, record(to).name, (uintptr_t)record(to).address);
#endif
        panic("lockdep: dependency cycle detected");
    }
    if (g_edge_count == CONFIG_LOCKDEP_MAX_EDGES) { panic("lockdep: dependency edge capacity exhausted"); }
    g_edges[g_edge_count++] = edge_record{from, to};
}
}  // namespace
#endif

uint32_t allocate_identity(const void* address, const char* name) {
#ifndef NDEBUG
    table_guard guard;
    uint32_t identity;
    if (g_free_count > 0) {
        identity = g_free_list[--g_free_count];
    } else {
        if (g_next_identity > CONFIG_LOCKDEP_MAX_LOCKS) { panic("lockdep: registered lock capacity exhausted"); }
        identity = g_next_identity++;
    }
    g_locks[identity - 1]         = lock_record{};
    g_locks[identity - 1].address = address;
    g_locks[identity - 1].name    = name;
    return identity;
#else
    (void)address;
    (void)name;
    return 0;
#endif
}

void release_identity(uint32_t identity) {
#ifndef NDEBUG
    if (identity == 0) { return; }
    table_guard guard;
    // Drop edges naming this identity so a reused slot cannot inherit stale ordering.
    for (size_t i = 0; i < g_edge_count;) {
        if (g_edges[i].from == identity || g_edges[i].to == identity) {
            g_edges[i] = g_edges[--g_edge_count];
        } else {
            ++i;
        }
    }
    g_locks[identity - 1]       = lock_record{};
    g_free_list[g_free_count++] = identity;
#else
    (void)identity;
#endif
}

void acquired(const void* address, uint32_t identity) {
#ifndef NDEBUG
    table_guard guard;
    auto& context = current_execution_context();
    for (size_t i = 0; i < context.held_count; ++i) {
        if (context.held[i].address == address) { panic("lockdep: recursive lock acquisition"); }
        learn_edge(context.held[i].identity, identity);
    }
    if (context.held_count == CONFIG_LOCKDEP_MAX_HELD) { panic("lockdep: held-lock capacity exhausted"); }
    context.held[context.held_count++] = held_lock{address, identity};
    if (identity != 0) {
        auto& lock = record(identity);
        if (lock.owned) { panic("lockdep: lock already owned"); }
        lock.owner_cpu    = context.cpu_index;
        lock.owner_thread = context.thread_id;
        lock.owned        = true;
    }
#else
    (void)address;
    (void)identity;
#endif
}

void released(const void* address, uint32_t identity) {
#ifndef NDEBUG
    table_guard guard;
    auto& context = current_execution_context();
    if (context.held_count == 0 || context.held[context.held_count - 1].address != address) {
        panic("lockdep: unbalanced or out-of-order lock release");
    }
    --context.held_count;
    if (identity != 0) {
        auto& lock = record(identity);
        // By thread, not core: a mutex holder may be preempted and resume on another core.
        if (!lock.owned || lock.owner_thread != context.thread_id) { panic("lockdep: lock released by non-owner"); }
        lock.owned = false;
    }
#else
    (void)address;
    (void)identity;
#endif
}

void assert_not_owned(const void* address, uint32_t identity) {
#ifndef NDEBUG
    (void)address;
    table_guard guard;
    if (identity != 0 && record(identity).owned) { panic("lockdep: destroying an owned lock"); }
#else
    (void)address;
    (void)identity;
#endif
}

#if CONFIG_KERNEL_TESTING
void reset_for_testing() {
#ifndef NDEBUG
    g_next_identity = 1;
    g_edge_count    = 0;
    g_free_count    = 0;
    for (auto& lock : g_locks) { lock = {}; }
    current_execution_context().held_count = 0;
#endif
}
size_t edge_count_for_testing() {
#ifndef NDEBUG
    return g_edge_count;
#else
    return 0;
#endif
}
#endif

}  // namespace kernel::synchronization::lockdep
