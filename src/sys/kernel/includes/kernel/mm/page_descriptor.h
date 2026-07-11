#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/mm/page.h"

namespace kernel::mm {

class vmo;  // owner back-references are wired up in the VMO phase

// Lifecycle of a physical frame. MMIO frames are address holes, firmware
// ranges, and device windows -- not memory, never allocatable, excluded from
// usage accounting. WIRED frames are permanently pinned RAM (kernel image,
// descriptor array, zero page); ACTIVE frames are allocated and in use; FREE
// frames belong to the PMM; ZEROED frames are free and known-zero. INACTIVE
// is defined for the eviction era and unused this milestone.
enum class page_state : uint8_t {
    MMIO = 0,  // zero so the freshly zeroed array starts unallocatable
    WIRED,
    FREE,
    ZEROED,
    ACTIVE,
    INACTIVE,
};

// Per-frame metadata. Residency and CoW truth live here and in the VMO
// structures -- never in software-defined PTE bits.
struct page_descriptor {
    vmo* owner;            // owning VMO, nullptr while unowned
    uint64_t offset;       // page offset within the owner
    uint32_t share_count;  // CoW sharers; 0 while unowned or exclusively owned
    page_state state;
};
static_assert(sizeof(page_descriptor) <= 32, "page descriptors are per-frame; keep them small");

// Global flat array of page descriptors indexed by PFN, covering physical
// memory up to the highest usable/kernel frame. Allocated once at VMM init
// from contiguous PMM frames, addressed through the HHDM.
class page_descriptor_table {
   public:
    // Sizes the array from the highest end address across both range lists,
    // marks usable ranges FREE, wired (kernel) ranges WIRED, and the array's
    // own frames WIRED. Everything else stays MMIO (the zeroed default).
    bool init(const vm_page_region* usable, size_t usable_count, const vm_page_region* wired, size_t wired_count);

    bool initialized() const { return m_array != nullptr; }
    // One past the last covered physical address.
    vm_paddr_t coverage_end() const { return m_count * 0x1000; }

    // Descriptor for a physical address; nullptr when uninitialized or out of
    // coverage (holes above the tracked range, early boot).
    page_descriptor* lookup(vm_paddr_t paddr);
    const page_descriptor* lookup(vm_paddr_t paddr) const;

    // State transition helper that tolerates uninitialized/uncovered frames --
    // the PMM calls this on every alloc/free, including before VMM init.
    void set_state(vm_paddr_t paddr, page_state state);
    void mark_range(vm_paddr_t base, size_t pages, page_state state);

    // Linear scan; for shell/test observability, not hot paths.
    size_t count(page_state state) const;

   private:
    page_descriptor* m_array = nullptr;
    size_t m_count           = 0;  // descriptors == covered PFNs
};

extern page_descriptor_table g_page_descriptors;

}  // namespace kernel::mm
