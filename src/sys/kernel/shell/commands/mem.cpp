#include <kernel/shell/shell.h>

#if CONFIG_KERNEL_SHELL

#include <kernel/config.h>
#include <kernel/mm/early_heap.h>
#include <kernel/mm/object_arena.h>
#include <kernel/mm/page_descriptor.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/slab_heap.h>
#include <kernel/mm/vm_aspace.h>
#include <kernel/shell/output.h>

#include <ktl/fmt>

namespace {

using namespace kernel::mm;
using kernel::shell::ShellOutput;

constexpr size_t PAGE_SIZE = KERNEL_MINIMUM_PAGE_SIZE;

const char* human_bytes(char* buf, size_t size, uint64_t bytes) {
    constexpr uint64_t KIB = 1024;
    constexpr uint64_t MIB = KIB * 1024;
    constexpr uint64_t GIB = MIB * 1024;
    uint64_t unit          = 1;
    const char* suffix     = "B";
    if (bytes >= GIB) {
        unit   = GIB;
        suffix = "GiB";
    } else if (bytes >= MIB) {
        unit   = MIB;
        suffix = "MiB";
    } else if (bytes >= KIB) {
        unit   = KIB;
        suffix = "KiB";
    }
    uint64_t tenths = bytes * 10 / unit;
    if (tenths % 10 == 0 || tenths / 10 >= 100) {
        ktl::format::format_to_buffer_raw(buf, size, "{0} {1}", tenths / 10, suffix);
    } else {
        ktl::format::format_to_buffer_raw(buf, size, "{0}.{1} {2}", tenths / 10, tenths % 10, suffix);
    }
    return buf;
}

const char* human_pages(char* buf, size_t size, uint64_t pages) { return human_bytes(buf, size, pages * PAGE_SIZE); }

void mem_handler(int, const ktl::string_view[], ShellOutput& output) {
    auto pmm = g_page_frame_allocator.stats();
    char free[24], total[24], used[24], reserved[24], heap_used[24], heap_total[24];
    size_t used_pages = pmm.total_pages - pmm.free_pages - pmm.reserved_pages;

    output.print("physical: {0} free / {1} total, {2} used, {3} reserved\n",
                 human_pages(free, sizeof(free), pmm.free_pages), human_pages(total, sizeof(total), pmm.total_pages),
                 human_pages(used, sizeof(used), used_pages),
                 human_pages(reserved, sizeof(reserved), pmm.reserved_pages));
    output.print("pmm: {0} zeroed, {1} dirty, {2} allocations, {3} frees, {4} failures\n", pmm.zeroed_pooled, pmm.dirty,
                 pmm.alloc_count, pmm.free_count, pmm.alloc_failures);

    if (g_page_descriptors.initialized()) {
        output.print("pages: wired {0}, active {1}, inactive {2}, free {3}, zeroed {4}, mmio {5}\n",
                     g_page_descriptors.count(page_state::WIRED), g_page_descriptors.count(page_state::ACTIVE),
                     g_page_descriptors.count(page_state::INACTIVE), g_page_descriptors.count(page_state::FREE),
                     g_page_descriptors.count(page_state::ZEROED), g_page_descriptors.count(page_state::MMIO));
    }

    auto heap = g_early_heap.stats();
    output.print("early heap: {0} used / {1} total, {2} blocks, {3} allocations, {4} frees\n",
                 human_bytes(heap_used, sizeof(heap_used), heap.used_bytes),
                 human_bytes(heap_total, sizeof(heap_total), heap.used_bytes + heap.free_bytes), heap.blocks,
                 heap.alloc_calls, heap.free_calls);

    if (heap_slab_active()) {
        auto slab = g_slab_heap.stats();
        output.print("slab heap: {0} allocations, {1} frees, {2} failures, large: {3} allocs / {4} pages\n",
                     slab.alloc_calls, slab.free_calls, slab.failures, slab.large_allocs, slab.large_pages);
        for (const auto& cls : slab.classes) {
            if (cls.slabs == 0) { continue; }
            output.print("  {0}B: {1} slabs, {2} live, {3} free slots\n", cls.object_size, cls.slabs, cls.live_objects,
                         cls.free_slots);
        }
    }

    for (auto* arena = object_arena::first_arena(); arena != nullptr; arena = arena->next_arena()) {
        auto a = arena->stats();
        output.print("arena {0}: {1}B slots, {2} slabs, {3}/{4} live, {5} allocs, {6} frees, {7} failures\n", a.name,
                     a.slot_size, a.slabs, a.live, a.capacity, a.alloc_calls, a.free_calls, a.failures);
    }

    vm_aspace& aspace = kernel_aspace();
    if (aspace.has_root()) {
        output.print("kernel aspace: [0x{0:p}, 0x{1:p}), {2} faults\n", aspace.root().base(),
                     aspace.root().base() + aspace.root().size(), aspace.fault_count());
    } else {
        output.print("kernel aspace: not initialized\n");
    }
}

}  // namespace

KSHELL_COMMAND(mem, "mem", "Show a plain memory summary", mem_handler);

#endif  // CONFIG_KERNEL_SHELL
