#include <kernel/config.h>
#include <kernel/mm/page_descriptor.h>
#include <kernel/mm/physmap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/slab_heap.h>
#include <kernel/panic.h>

namespace kernel::mm {

uintptr_t heap_pages_alloc(size_t pages) {
    if (pages == 1) {
        auto frame = g_page_frame_allocator.alloc();
        return frame.has_value() ? direct_map_address(physical_address(frame.value())) : 0;
    }
    // ponytail: multi-page runs carve untouched region tails and are returned below as single
    // pages, so heavy multi-page alloc/free traffic slowly consumes contiguous capacity; a PMM
    // that tracks free runs (buddy or equivalent) lifts this ceiling.
    auto frame = g_page_frame_allocator.alloc_contiguous(pages);
    return frame.has_value() ? direct_map_address(physical_address(frame.value())) : 0;
}

void heap_pages_free(uintptr_t base, size_t pages) {
    for (size_t i = 0; i < pages; ++i) {
        g_page_frame_allocator.free(direct_map_physical(reinterpret_cast<void*>(base)).value() +
                                    i * KERNEL_MINIMUM_PAGE_SIZE);
    }
}

// Large-run lengths live in the first frame's page descriptor: `owner` stays null for heap frames,
// so the otherwise-unused `offset` field carries a tag word plus the page count. The tag is what
// authenticates a page-aligned free -- a pointer whose descriptor lacks it was never a heap run.
namespace {
constexpr uint64_t HEAP_RUN_TAG  = 0x50414548ull << 32;  // "HEAP"
constexpr uint64_t RUN_TAG_MASK  = 0xFFFFFFFFull << 32;
constexpr uint64_t RUN_PAGE_MASK = 0xFFFFFFFFull;

page_descriptor* run_descriptor(uintptr_t base) {
    page_descriptor* descriptor = g_page_descriptors.lookup(direct_map_physical(reinterpret_cast<void*>(base)).value());
    if (descriptor == nullptr) { panic("heap: large run page has no descriptor"); }
    return descriptor;
}
}  // namespace

void heap_pages_note_run(uintptr_t base, size_t pages) {
    run_descriptor(base)->offset = HEAP_RUN_TAG | (pages & RUN_PAGE_MASK);
}

size_t heap_pages_take_run(uintptr_t base) {
    page_descriptor* descriptor = run_descriptor(base);
    if ((descriptor->offset & RUN_TAG_MASK) != HEAP_RUN_TAG) {
        panic("heap: free of a pointer the heap never produced");
    }
    size_t pages       = descriptor->offset & RUN_PAGE_MASK;
    descriptor->offset = 0;
    return pages;
}

}  // namespace kernel::mm
