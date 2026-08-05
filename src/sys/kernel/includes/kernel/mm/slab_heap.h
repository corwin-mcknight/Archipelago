#pragma once

#include <kernel/config.h>
#include <kernel/synchronization/spinlock.h>
#include <stddef.h>
#include <stdint.h>

// The kernel heap: size-class slabs over physmap pages.
//
// Decisions this encodes, in order of importance:
// - Allocation failure is a value, not a panic. alloc() returns nullptr when pages run out, which
//   is what makes every downstream errc::oom path real. Callers that cannot continue say so with
//   an explicit expect/panic at the call site.
// - Physmap-backed: slab pages are PMM pages reached through the physmap, so the heap needs no
//   virtual-address management, no kernel page-table mutation, and no TLB interplay. The trade is
//   no guard pages between allocations; a debug configuration can route suspect classes through a
//   mapped region later.
// - Interrupt context never allocates or frees. Asserted on every call. Drivers preallocate at
//   bind time; this rule is what keeps the heap free of reserved-pool machinery.

namespace kernel::mm {

// Page source seam: kernel builds implement these over the PMM through the physmap
// (mm/heap_pages.cpp); the host runner supplies aligned_alloc-backed stubs so hosted tests
// exercise the slab logic under ASan. Returns a page-aligned run, or 0 when exhausted.
uintptr_t heap_pages_alloc(size_t pages);
void heap_pages_free(uintptr_t base, size_t pages);

// Large-run bookkeeping lives beside the pages, not inside them: the kernel records a tagged run
// length in the first frame's page descriptor (the descriptor array is the metadata slab, in
// Umbra terms), so large allocations own every byte of their pages and come out page-aligned.
// take validates the tag and panics on a pointer the heap never produced, then clears the record.
void heap_pages_note_run(uintptr_t base, size_t pages);
size_t heap_pages_take_run(uintptr_t base);

struct slab_class_stats {
    size_t object_size;
    size_t slabs;
    size_t live_objects;
    size_t free_slots;
};

struct slab_heap_stats {
    slab_class_stats classes[7];
    size_t large_allocs;  // live allocations above the largest class
    size_t large_pages;   // pages those occupy
    uint64_t alloc_calls;
    uint64_t free_calls;
    uint64_t failures;
};

class slab_heap {
   public:
    static constexpr size_t CLASS_COUNT     = 7;  // 16, 32, 64, 128, 256, 512, 1024
    static constexpr size_t MAX_CLASS_BYTES = 1024;
    // Slab slots deliver up to cache-line alignment; anything stricter routes to the large path,
    // whose runs are naturally page-aligned. Alignment above a page has no client and panics.
    static constexpr size_t MAX_SLAB_ALIGN  = 64;
    static constexpr size_t MAX_ALIGN       = KERNEL_MINIMUM_PAGE_SIZE;

    // nullptr on exhaustion. align must be a power of two <= MAX_ALIGN.
    void* alloc(size_t size, size_t align = sizeof(void*));
    void free(void* ptr);

    slab_heap_stats stats();

   private:
    struct slab_page;

    struct class_state {
        slab_page* partial = nullptr;  // slabs with at least one free slot, doubly linked
        size_t slab_count  = 0;
        size_t live        = 0;
    };

    void* alloc_large(size_t size);
    void* take_slot(class_state& cls, slab_page* slab, size_t class_size);
    slab_page* new_slab(size_t class_index);
    void unlink_partial(class_state& cls, slab_page* slab);

    class_state m_classes[CLASS_COUNT];
    size_t m_large_allocs  = 0;
    size_t m_large_pages   = 0;
    uint64_t m_alloc_calls = 0;
    uint64_t m_free_calls  = 0;
    uint64_t m_failures    = 0;
    // ponytail: one lock for the whole heap; split per class when SMP contention is measurable.
    kernel::synchronization::spinlock m_lock;
};

extern slab_heap g_slab_heap;

// Flip operator new/delete from the early heap to the slab heap. Called once from init_memory
// after the PMM is live; never unflipped. Pointers handed out by the early heap before the flip
// are routed back to it on free by address range.
void heap_activate();
bool heap_slab_active();

}  // namespace kernel::mm
