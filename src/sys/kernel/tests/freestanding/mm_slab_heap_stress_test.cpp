#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/log.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/slab_heap.h>
#include <kernel/sched/scheduler.h>
#include <kernel/sched/thread.h>
#include <std/string.h>

#include <ktl/atomic>

using namespace kernel::mm;
using namespace kernel::sched;

KTEST_MODULE("mm/slab_heap_stress");

namespace {

struct hammer_ctx {
    uint64_t seed;
    ktl::atomic<uint32_t>* errors;
};

uint64_t xorshift(uint64_t& s) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

// Each worker churns its own pocket of allocations with content tags, yielding often enough that
// the timer preempts workers inside the allocator: the point is the heap spinlock and the
// two-phase alloc under real interleaving, not throughput.
void heap_hammer_thread(void* arg) {
    auto* ctx  = static_cast<hammer_ctx*>(arg);
    uint64_t s = ctx->seed;

    struct pocket {
        void* ptr;
        size_t size;
        uint8_t tag;
    };
    pocket slots[24] = {};

    for (size_t op = 0; op < 6000; ++op) {
        if (op % 1000 == 0) { g_log.info("hammer {0}: op {1}", ctx->seed & 0xFF, op); }
        size_t index = xorshift(s) % 24;
        if (slots[index].ptr != nullptr) {
            auto* bytes = static_cast<uint8_t*>(slots[index].ptr);
            if (bytes[0] != slots[index].tag || bytes[slots[index].size - 1] != slots[index].tag) {
                ctx->errors->fetch_add(1, ktl::memory_order::relaxed);
            }
            g_slab_heap.free(slots[index].ptr);
            slots[index].ptr = nullptr;
        } else {
            // Mostly slab-class sizes, a steady trickle of one-page and multi-page large runs.
            size_t shape = xorshift(s) % 16;
            size_t size  = shape == 0 ? 4097 + (s % 8192) : (shape < 3 ? 1025 + (s % 3000) : 1 + (s % 1400));
            void* p      = g_slab_heap.alloc(size);
            if (p == nullptr) {
                ctx->errors->fetch_add(1, ktl::memory_order::relaxed);
                break;
            }
            auto tag = static_cast<uint8_t>(xorshift(s) | 1);
            memset(p, tag, size);
            slots[index] = {p, size, tag};
        }
        if ((op & 63) == 0) { yield(); }
    }

    for (auto& slot : slots) {
        if (slot.ptr == nullptr) { continue; }
        auto* bytes = static_cast<uint8_t*>(slot.ptr);
        if (bytes[0] != slot.tag || bytes[slot.size - 1] != slot.tag) {
            ctx->errors->fetch_add(1, ktl::memory_order::relaxed);
        }
        g_slab_heap.free(slot.ptr);
    }
}

}  // namespace

// Three workers hammer the heap concurrently under timer preemption, with per-allocation content
// tags as the corruption oracle. Afterwards the heap must be back to its idle shape and the
// borrowed PMM pages returned.
KTEST_CASE_INTEGRATION(slab_heap_concurrent_hammer) {
    size_t free_before = g_page_frame_allocator.free_pages();
    ktl::atomic<uint32_t> errors{0};

    hammer_ctx contexts[3] = {
        {0x9E3779B97F4A7C15ull, &errors},
        {0xD1B54A32D192ED03ull, &errors},
        {0x94D049BB133111EBull, &errors},
    };
    ktl::ref<Thread> workers[3];
    for (size_t i = 0; i < 3; ++i) {
        KTEST_UNWRAP(t, spawn("heap-hammer", heap_hammer_thread, &contexts[i]));
        workers[i] = t;
    }
    for (auto& worker : workers) {
        worker->wait_signals(Thread::SIGNAL_TERMINATED);
        g_log.info("hammer: worker joined");
    }

    KTEST_EXPECT_EQUAL(errors.load(ktl::memory_order::relaxed), 0u);

    // Other kernel threads allocate from the same classes, so per-class shapes and exact-zero
    // liveness are not provable here (the hosted stress suite owns those). What is provable: no
    // allocation ever failed, and the borrowed pages came back to within the per-class spares
    // plus a small allowance for unrelated kernel activity.
    KTEST_EXPECT_TRUE(g_slab_heap.stats().failures == 0);
    size_t free_after = g_page_frame_allocator.free_pages();
    KTEST_EXPECT_TRUE(free_before <= free_after + slab_heap::CLASS_COUNT + 16);
}

#endif
