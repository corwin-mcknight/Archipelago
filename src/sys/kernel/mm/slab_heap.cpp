#include <kernel/mm/slab_heap.h>
#include <kernel/panic.h>
#include <kernel/synchronization/execution_context.h>
#include <kernel/synchronization/guard.h>
#include <std/new.h>

namespace kernel::mm {

slab_heap g_slab_heap;

namespace {

constexpr size_t PAGE                                = KERNEL_MINIMUM_PAGE_SIZE;
// Slab pages begin with a 64-byte tagged header; slots start at 64 and are never page-aligned.
// Large runs carry no header at all -- their length lives in the first frame's page descriptor
// (heap_pages_note_run/take_run) -- so a page-aligned pointer always means a large run and free()
// classifies by alignment alone. 64 keeps the first slot aligned for every class.
constexpr size_t HEADER_BYTES                        = 64;
constexpr uint32_t SLAB_MAGIC                        = 0x42414c53;  // "SLAB"

constexpr size_t CLASS_SIZES[slab_heap::CLASS_COUNT] = {16, 32, 64, 128, 256, 512, 1024};

size_t class_for(size_t bytes) {
    for (size_t i = 0; i < slab_heap::CLASS_COUNT; ++i) {
        if (bytes <= CLASS_SIZES[i]) { return i; }
    }
    __builtin_unreachable();
}

// Only interrupt-free contexts may touch the heap: handlers preallocate at bind time instead.
// This single rule is what lets the heap run without reserved pools or IRQ-safe locking. A hard
// check, not a debug assert -- heap invariants are the kind of thing that must not compile out.
void check_not_interrupt() {
    if (kernel::synchronization::current_execution_context().interrupt_depth != 0) {
        panic("heap: allocation from interrupt context");
    }
}

}  // namespace

// Slot freeing threads a uint16 next-offset through the free slots themselves; never-used slots
// are tracked by a bump offset so a fresh slab initializes nothing but this header. The bitmap
// carries one bit per possible slot (at most (4096-64)/16 == 252) recording which are live, which
// is what makes double frees and frees of never-carved slots deterministic panics instead of
// freelist corruption.
struct slab_heap::slab_page {
    uint32_t magic;
    uint16_t class_index;
    uint16_t live;
    uint16_t free_head;    // byte offset of first freed slot, 0 = none
    uint16_t next_uninit;  // byte offset of next never-used slot, 0 = slab fully carved
    slab_page* next;
    slab_page* prev;
    uint64_t live_bits[4];

    bool full() const { return free_head == 0 && next_uninit == 0; }

    bool test_live(size_t slot) const { return (live_bits[slot >> 6] >> (slot & 63)) & 1; }
    void mark_live(size_t slot) { live_bits[slot >> 6] |= 1ull << (slot & 63); }
    void clear_live(size_t slot) { live_bits[slot >> 6] &= ~(1ull << (slot & 63)); }
};

slab_heap::slab_page* slab_heap::new_slab(size_t class_index) {
    static_assert(sizeof(slab_page) <= HEADER_BYTES);
    uintptr_t base = heap_pages_alloc(1);
    if (base == 0) { return nullptr; }
    // Placement-new starts the header's lifetime in the raw page (plain member writes through a
    // cast pointer would be lifetime UB); value-init zeroes the bitmap.
    auto* slab        = new (reinterpret_cast<void*>(base)) slab_page{};
    slab->magic       = SLAB_MAGIC;
    slab->class_index = static_cast<uint16_t>(class_index);
    slab->next_uninit = HEADER_BYTES;
    return slab;
}

void slab_heap::unlink_partial(class_state& cls, slab_page* slab) {
    if (slab->prev != nullptr) { slab->prev->next = slab->next; }
    if (slab->next != nullptr) { slab->next->prev = slab->prev; }
    if (cls.partial == slab) { cls.partial = slab->next; }
    slab->next = nullptr;
    slab->prev = nullptr;
}

// The heap lock is a leaf: page acquisition and release happen outside it, so holding any other
// lock while allocating can never form a cycle through the heap into the PMM's locking.

void* slab_heap::take_slot(class_state& cls, slab_page* slab, size_t class_size) {
    uintptr_t base = reinterpret_cast<uintptr_t>(slab);
    uint16_t offset;
    if (slab->free_head != 0) {
        offset          = slab->free_head;
        slab->free_head = *reinterpret_cast<uint16_t*>(base + offset);
    } else {
        offset            = slab->next_uninit;
        size_t following  = offset + class_size;
        slab->next_uninit = (following + class_size <= PAGE) ? static_cast<uint16_t>(following) : 0;
    }
    slab->mark_live((offset - HEADER_BYTES) / class_size);
    slab->live++;
    cls.live++;
    if (slab->full()) { unlink_partial(cls, slab); }
    return reinterpret_cast<void*>(base + offset);
}

void* slab_heap::alloc(size_t size, size_t align) {
    check_not_interrupt();
    if (align == 0 || (align & (align - 1)) != 0 || align > MAX_ALIGN) { panic("heap: unsupported alignment"); }
    if (size == 0) { size = 1; }
    size_t need = size > align ? size : align;
    if (need > MAX_CLASS_BYTES || align > MAX_SLAB_ALIGN) { return alloc_large(size); }

    size_t index      = class_for(need);
    size_t class_size = CLASS_SIZES[index];
    class_state& cls  = m_classes[index];
    {
        kernel::synchronization::critical_lock_guard guard(m_lock);
        m_alloc_calls++;
        if (cls.partial != nullptr) { return take_slot(cls, cls.partial, class_size); }
    }

    // No partial slab: fetch a page lock-free (the PMM synchronizes itself), then link and carve.
    // If another thread replenished the class meanwhile, the extra slab is just capacity.
    slab_page* fresh = new_slab(index);
    if (fresh == nullptr) {
        kernel::synchronization::critical_lock_guard guard(m_lock);
        m_failures++;
        return nullptr;
    }
    kernel::synchronization::critical_lock_guard guard(m_lock);
    fresh->next = cls.partial;
    if (cls.partial != nullptr) { cls.partial->prev = fresh; }
    cls.partial = fresh;
    cls.slab_count++;
    return take_slot(cls, fresh, class_size);
}

void* slab_heap::alloc_large(size_t size) {
    // A size whose page rounding wraps, or whose page count exceeds what the descriptor's 32-bit
    // run field can record, is an impossible request: fail it as exhaustion rather than letting
    // pages==0 reach the PMM (alloc_contiguous(0) historically returned a live frame) or a
    // truncated length reach note_run.
    size_t pages = (size + PAGE - 1) / PAGE;
    if (size > SIZE_MAX - (PAGE - 1) || pages > 0xFFFFFFFFull) {
        kernel::synchronization::critical_lock_guard guard(m_lock);
        m_alloc_calls++;
        m_failures++;
        return nullptr;
    }

    uintptr_t base = heap_pages_alloc(pages);
    if (base != 0) { heap_pages_note_run(base, pages); }

    kernel::synchronization::critical_lock_guard guard(m_lock);
    m_alloc_calls++;
    if (base == 0) {
        m_failures++;
        return nullptr;
    }
    m_large_allocs++;
    m_large_pages += pages;
    return reinterpret_cast<void*>(base);
}

void slab_heap::free(void* ptr) {
    check_not_interrupt();
    uintptr_t address = reinterpret_cast<uintptr_t>(ptr);

    // Slab slots start past the in-page header, so a page-aligned pointer can only be a large
    // run. Its length comes from the page descriptor, which also authenticates the pointer --
    // take_run panics on an address the heap never handed out. It runs under m_lock (it takes no
    // lock of its own, so the leaf property holds) so that two racing frees of the same run
    // serialize: the loser sees the cleared tag and panics as a double free instead of both
    // returning the frames twice.
    if ((address & (PAGE - 1)) == 0) {
        size_t pages;
        {
            kernel::synchronization::critical_lock_guard guard(m_lock);
            pages = heap_pages_take_run(address);
            m_free_calls++;
            m_large_allocs--;
            m_large_pages -= pages;
        }
        heap_pages_free(address, pages);
        return;
    }

    uintptr_t page         = address & ~(PAGE - 1);
    uintptr_t release_base = 0;
    size_t release_pages   = 0;
    {
        kernel::synchronization::critical_lock_guard guard(m_lock);
        m_free_calls++;

        auto* slab = reinterpret_cast<slab_page*>(page);
        if (slab->magic != SLAB_MAGIC) { panic("heap: free of a pointer the heap never produced"); }
        if (slab->class_index >= CLASS_COUNT) { panic("heap: slab header corrupted"); }
        class_state& cls  = m_classes[slab->class_index];
        bool was_full     = slab->full();

        // A slab pointer must sit exactly on a slot boundary with the whole slot inside the page;
        // anything else is a corrupted or interior pointer that would poison the freelist if
        // threaded onto it. The page tail has stride-aligned offsets that were never carved
        // (e.g. 3136 in the 1024 class), which the upper bound rejects.
        size_t class_size = CLASS_SIZES[slab->class_index];
        size_t offset     = address - page;
        if (offset < HEADER_BYTES || offset > PAGE - class_size || (offset - HEADER_BYTES) % class_size != 0) {
            panic("heap: slab free not on a slot boundary");
        }
        size_t slot = (offset - HEADER_BYTES) / class_size;
        if (!slab->test_live(slot)) { panic("heap: double free, or free of a slot never allocated"); }
        slab->clear_live(slot);

        *reinterpret_cast<uint16_t*>(address) = slab->free_head;
        slab->free_head                       = static_cast<uint16_t>(offset);
        slab->live--;
        cls.live--;

        if (was_full) {
            slab->next = cls.partial;
            slab->prev = nullptr;
            if (cls.partial != nullptr) { cls.partial->prev = slab; }
            cls.partial = slab;
        }

        // ponytail: keep the last partial slab as a hot spare, release any other fully-free
        // slab; a per-class spare count is the knob if churn at a class boundary ever shows.
        if (slab->live == 0 && !(cls.partial == slab && slab->next == nullptr)) {
            unlink_partial(cls, slab);
            cls.slab_count--;
            release_base  = page;
            release_pages = 1;
        }
    }
    if (release_pages != 0) { heap_pages_free(release_base, release_pages); }
}

slab_heap_stats slab_heap::stats() {
    kernel::synchronization::critical_lock_guard guard(m_lock);
    slab_heap_stats out{};
    for (size_t i = 0; i < CLASS_COUNT; ++i) {
        size_t per_slab = (PAGE - HEADER_BYTES) / CLASS_SIZES[i];
        out.classes[i]  = {CLASS_SIZES[i], m_classes[i].slab_count, m_classes[i].live,
                           m_classes[i].slab_count * per_slab - m_classes[i].live};
    }
    out.large_allocs = m_large_allocs;
    out.large_pages  = m_large_pages;
    out.alloc_calls  = m_alloc_calls;
    out.free_calls   = m_free_calls;
    out.failures     = m_failures;
    return out;
}

}  // namespace kernel::mm
