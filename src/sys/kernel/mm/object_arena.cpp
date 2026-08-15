#include <kernel/config.h>
#include <kernel/mm/object_arena.h>
#include <kernel/mm/slab_heap.h>
#include <kernel/panic.h>
#include <kernel/sched/thread.h>
#include <kernel/synchronization/execution_context.h>
#include <kernel/synchronization/guard.h>
#include <std/new.h>

namespace kernel::mm {

namespace {

constexpr size_t PAGE          = KERNEL_MINIMUM_PAGE_SIZE;
constexpr uint32_t ARENA_MAGIC = 0x41524E41;  // 'ARNA'

object_arena* g_arena_list     = nullptr;  // zero-initialized POD: safe before any constructor runs

size_t align_up(size_t value, size_t align) { return (value + align - 1) & ~(align - 1); }

}  // namespace

// One page per slab: a header with backref and live bitmap, then the slots. The bitmap is the
// whole allocation state -- a set bit is a live slot, a clear bit a free one -- and because free
// slots are zero there is nothing else to maintain.
struct object_arena::arena_slab {
    uint32_t magic;
    uint32_t live;
    object_arena* owner;
    arena_slab* next;
    arena_slab* prev;
    // Bitmap words follow the header; slots follow those at the arena's slots offset.
    uint64_t* bits() { return reinterpret_cast<uint64_t*>(this + 1); }

    bool test_live(size_t slot) { return (bits()[slot >> 6] >> (slot & 63)) & 1; }
    void mark_live(size_t slot) { bits()[slot >> 6] |= 1ull << (slot & 63); }
    void clear_live(size_t slot) { bits()[slot >> 6] &= ~(1ull << (slot & 63)); }
};

object_arena* object_arena::first_arena() { return g_arena_list; }

object_arena::object_arena(const char* name, size_t object_size, size_t align) : m_name(name) {
    if (align == 0 || (align & (align - 1)) != 0 || align > 64) { panic("arena: unsupported alignment"); }
    m_object_size = object_size == 0 ? 1 : object_size;
    m_slot_size   = align_up(m_object_size, align);

    // Solve for the slot count: header, then bitmap words for that many slots, then the slots
    // themselves, all inside one page. Start from the no-bitmap upper bound and shrink.
    size_t slots  = (PAGE - sizeof(arena_slab)) / m_slot_size;
    while (slots > 0) {
        size_t words  = (slots + 63) / 64;
        size_t offset = align_up(sizeof(arena_slab) + words * sizeof(uint64_t), align);
        if (offset + slots * m_slot_size <= PAGE) {
            m_bitmap_words = words;
            m_slots_offset = offset;
            break;
        }
        slots--;
    }
    if (slots == 0) { panic("arena: object does not fit a one-page slab"); }
    m_slots_per_slab = slots;

    m_next           = g_arena_list;
    g_arena_list     = this;
}

object_arena::~object_arena() {
    object_arena** link = &g_arena_list;
    while (*link != nullptr && *link != this) { link = &(*link)->m_next; }
    if (*link == this) { *link = m_next; }
}

object_arena::arena_slab* object_arena::new_slab() {
    uintptr_t base = heap_pages_alloc(1);
    if (base == 0) { return nullptr; }
    // The zero-on-free invariant needs every slot zero from the start. Kernel pages arrive zeroed
    // from the PMM already; the host stub's do not, and one memset per new slab keeps the
    // invariant unconditional instead of trusted.
    __builtin_memset(reinterpret_cast<void*>(base), 0, PAGE);
    auto* slab  = new (reinterpret_cast<void*>(base)) arena_slab{};
    slab->magic = ARENA_MAGIC;
    slab->owner = this;
    return slab;
}

void object_arena::unlink_partial(arena_slab* slab) {
    if (slab->prev != nullptr) { slab->prev->next = slab->next; }
    if (slab->next != nullptr) { slab->next->prev = slab->prev; }
    if (m_partial == slab) { m_partial = slab->next; }
    slab->next = nullptr;
    slab->prev = nullptr;
}

void* object_arena::take_slot(arena_slab* slab) {
    for (size_t word = 0; word < m_bitmap_words; word++) {
        uint64_t bits = slab->bits()[word];
        if (bits == ~0ull) { continue; }
        size_t slot = word * 64 + static_cast<size_t>(__builtin_ctzll(~bits));
        if (slot >= m_slots_per_slab) { break; }  // trailing bits past the last real slot
        slab->mark_live(slot);
        slab->live++;
        m_live++;
        if (slab->live == m_slots_per_slab) { unlink_partial(slab); }
        return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(slab) + m_slots_offset + slot * m_slot_size);
    }
    panic("arena: partial slab had no free slot");
}

void* object_arena::alloc() {
    check_not_interrupt();
    {
        kernel::synchronization::critical_lock_guard guard(m_lock);
        m_alloc_calls++;
        if (m_partial != nullptr) { return take_slot(m_partial); }
    }

    // No partial slab: fetch a page outside the lock (the page source synchronizes itself). If
    // another thread replenished meanwhile, the extra slab is just capacity.
    arena_slab* fresh = new_slab();
    if (fresh == nullptr) {
        kernel::synchronization::critical_lock_guard guard(m_lock);
        m_failures++;
        return nullptr;
    }
    kernel::synchronization::critical_lock_guard guard(m_lock);
    fresh->next = m_partial;
    if (m_partial != nullptr) { m_partial->prev = fresh; }
    m_partial = fresh;
    m_slab_count++;
    return take_slot(fresh);
}

void object_arena::free(void* ptr) {
    check_not_interrupt();
    if (ptr == nullptr) { return; }

    uintptr_t address = reinterpret_cast<uintptr_t>(ptr);
    auto* slab        = reinterpret_cast<arena_slab*>(address & ~(PAGE - 1));
    if (slab->magic != ARENA_MAGIC || slab->owner != this) { panic("arena: free of foreign pointer"); }
    uintptr_t offset = address - reinterpret_cast<uintptr_t>(slab) - m_slots_offset;
    if (address < reinterpret_cast<uintptr_t>(slab) + m_slots_offset || offset % m_slot_size != 0) {
        panic("arena: free of interior pointer");
    }
    size_t slot = offset / m_slot_size;
    if (slot >= m_slots_per_slab) { panic("arena: free past slab"); }

    bool release_page = false;
    {
        kernel::synchronization::critical_lock_guard guard(m_lock);
        m_free_calls++;
        if (!slab->test_live(slot)) { panic("arena: double free"); }
        // Zero-on-free is the arena's contract: scrub before the slot becomes claimable.
        __builtin_memset(ptr, 0, m_slot_size);
        slab->clear_live(slot);
        slab->live--;
        m_live--;
        if (slab->live + 1 == m_slots_per_slab && slab->live != 0) {
            // Was full: back onto the partial list.
            slab->next = m_partial;
            if (m_partial != nullptr) { m_partial->prev = slab; }
            m_partial = slab;
        } else if (slab->live == 0) {
            // Empty: give the page back. Unlink from partial (a one-slot slab can go full ->
            // empty without ever sitting on the list; unlink_partial's null checks cover that).
            unlink_partial(slab);
            m_slab_count--;
            release_page = true;
        }
    }
    if (release_page) { heap_pages_free(reinterpret_cast<uintptr_t>(slab), 1); }
}

object_arena_stats object_arena::stats() {
    kernel::synchronization::critical_lock_guard guard(m_lock);
    object_arena_stats s;
    s.name        = m_name;
    s.object_size = m_object_size;
    s.slot_size   = m_slot_size;
    s.slabs       = m_slab_count;
    s.live        = m_live;
    s.capacity    = m_slab_count * m_slots_per_slab;
    s.alloc_calls = m_alloc_calls;
    s.free_calls  = m_free_calls;
    s.failures    = m_failures;
    return s;
}

// Client arenas and their types' allocation operators live here (not in the clients' own
// translation units) so the host test runner, which builds only a subset of kernel sources,
// always links the symbols the client headers declare.

object_arena g_thread_arena("thread", sizeof(kernel::sched::Thread), alignof(kernel::sched::Thread));

}  // namespace kernel::mm

namespace kernel::sched {

void* Thread::operator new(size_t size, const std::nothrow_t&) noexcept {
    return size <= sizeof(Thread) ? kernel::mm::g_thread_arena.alloc() : nullptr;
}
void Thread::operator delete(void* ptr) { kernel::mm::g_thread_arena.free(ptr); }

}  // namespace kernel::sched
