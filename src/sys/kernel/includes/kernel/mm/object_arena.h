#pragma once

#include <kernel/synchronization/spinlock.h>
#include <stddef.h>
#include <stdint.h>

// AUMI per-type arenas: named caches of exact-size, always-zero slots for one kernel type each,
// layered over the slab heap's page seam (heap_pages_alloc), so hosted tests exercise them under
// ASan through the same stubs. What distinguishes an arena from the general heap:
//
// - Exact-size slots, not power-of-two classes: a 920-byte object costs 920 bytes (rounded to its
//   alignment), not 1024.
// - Zero-on-free: free() scrubs the slot immediately, so freed objects' contents never linger and
//   every slot an arena hands out is already zero. Allocation and deallocation reduce to bitmap
//   operations -- there is no freelist threaded through the slots, because the slots stay zero.
// - The same safety rules as the heap: no allocation from interrupt context, the arena lock is a
//   leaf, and a free of a pointer the arena does not own -- wrong arena, interior, double, never
//   allocated -- is a deterministic panic.
//
// Arenas register themselves on a global list at construction for introspection (`mem` in the
// kernel shell). Remaining AUMI phases -- poisoning/redzones, guard-page debug mode, per-CPU
// magazines -- layer on top of this.

namespace kernel::mm {

struct object_arena_stats {
    const char* name;
    size_t object_size;
    size_t slot_size;
    size_t slabs;
    size_t live;
    size_t capacity;  // total slots across current slabs
    uint64_t alloc_calls;
    uint64_t free_calls;
    uint64_t failures;
};

class object_arena {
   public:
    // name must be a string literal. object_size must fit a one-page slab after its header;
    // align must be a power of two <= 64 (stricter alignment has no arena client).
    object_arena(const char* name, size_t object_size, size_t align);
    // Unregisters. Kernel arenas are globals that never die; this exists for test-local arenas,
    // which would otherwise leave dangling registry entries.
    ~object_arena();

    object_arena(const object_arena&)            = delete;
    object_arena& operator=(const object_arena&) = delete;

    // A zeroed slot, or nullptr on page exhaustion.
    void* alloc();
    // Scrubs the slot to zero and recycles it. Panics on any pointer this arena does not own.
    void free(void* ptr);

    object_arena_stats stats();

    // Registry for introspection, threaded at construction; boot constructs arenas before any
    // second thread exists, so the list needs no lock.
    object_arena* next_arena() const { return m_next; }
    static object_arena* first_arena();

   private:
    struct arena_slab;

    arena_slab* new_slab();
    void* take_slot(arena_slab* slab);

    const char* m_name;
    size_t m_object_size;
    size_t m_slot_size;
    size_t m_slots_per_slab;
    size_t m_bitmap_words;
    size_t m_slots_offset;
    arena_slab* m_partial  = nullptr;  // slabs with at least one free slot, doubly linked
    size_t m_slab_count    = 0;
    size_t m_live          = 0;
    uint64_t m_alloc_calls = 0;
    uint64_t m_free_calls  = 0;
    uint64_t m_failures    = 0;
    object_arena* m_next   = nullptr;
    kernel::synchronization::spinlock m_lock;
};

// The client arenas. Defined in object_arena.cpp beside the class so the host test runner links
// the operator new/delete each client type declares, whether or not the client's own translation
// unit is host-built.
extern object_arena g_thread_arena;

}  // namespace kernel::mm
