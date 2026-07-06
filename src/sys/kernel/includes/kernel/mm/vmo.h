#pragma once

#include <kernel/obj/object.h>
#include <kernel/obj/type_registry.h>
#include <kernel/obj/types.h>
#include <stddef.h>
#include <stdint.h>

#include <ktl/maybe>
#include <ktl/ref>
#include <ktl/result>
#include <ktl/vector>

#include "kernel/mm/page.h"
#include "kernel/mm/pager.h"

namespace kernel::mm {

class vm_aspace;
struct region_child;

// Virtual memory object: a resizable, page-granular container of memory whose
// pages materialize through a pager. Residency truth lives here (chunked
// frame index) and in the page descriptors -- never in PTE software bits.
class vmo : public obj::Object {
   public:
    DECLARE_OBJECT_TYPE(vmo, obj::type_ids::VMO)

    vmo(size_t pages, ktl::ref<pager> pgr);
    ~vmo() override;

    size_t size_pages() const { return m_pages; }
    size_t resident_pages() const { return m_resident; }
    uint64_t fill_count() const { return m_fills; }
    pager& backing_pager() { return *m_pager; }

    // Frame backing the given page, if resident.
    ktl::maybe<vm_paddr_t> resident_frame(uint64_t page) const;

    // Resolve a page to its backing frame, filling through the pager if
    // absent. The fault path's core; caller holds the VMM lock.
    ktl::result<vm_paddr_t> get_or_fill_page(uint64_t page);

    // Eager population / release without faulting. Range is [page, page+count).
    // Decommit zaps installed translations through the mapping back-refs and
    // returns pager-owned frames to the PMM.
    ktl::result<void> commit(uint64_t page, size_t count);
    ktl::result<void> decommit(uint64_t page, size_t count);

    // Resize. Growth extends the chunk index lazily (no allocation until
    // touch). Shrink wins over mappings: every translation past the new size
    // is zapped in every aspace, frames return to the PMM, and stale accesses
    // fault to an out-of-range error. Device VMOs reject resize.
    ktl::result<void> set_size(size_t new_pages);

    // Mapping back-refs, maintained by Region::map/unmap under the VMM lock.
    // Consumed by decommit and resize to find every translation of a page.
    void add_mapping(vm_aspace& aspace, region_child& binding);
    void remove_mapping(region_child& binding);
    size_t mapping_count() const { return m_mappings.size(); }

    static ktl::result<void> register_type(obj::TypeRegistry& registry) {
        return registry.register_type(TYPE_ID, "vmo", obj::RIGHT_READ | obj::RIGHT_WRITE, obj::RIGHT_READ);
    }

   private:
    struct mapping {
        vm_aspace* aspace;
        region_child* binding;
    };

    // Residency index: one entry per page, paddr-or-zero, packed in chunks of
    // 512 entries. Each chunk is one PMM frame (arrives pre-zeroed = all
    // absent), addressed through the HHDM; chunks allocate lazily on first
    // touch. The pointer vector itself is heap-backed and sized to the VMO.
    static constexpr size_t CHUNK_ENTRIES = 512;

    uint64_t* chunk_for(uint64_t page, bool allocate);
    ktl::result<void> fill_page(uint64_t page);
    void decommit_page(uint64_t page);
    void zap_page_mappings(uint64_t page);

    size_t m_pages;
    ktl::ref<pager> m_pager;
    ktl::vector<uint64_t*> m_chunks;
    ktl::vector<mapping> m_mappings;
    size_t m_resident = 0;
    uint64_t m_fills  = 0;
};

// Convenience factory: VMO backed by the shared anonymous (zero-fill) pager.
ktl::ref<vmo> create_anonymous_vmo(size_t pages);

// VMO wrapping a fixed physical window (MMIO or wired scratch) with the given
// cache mode. Never resizable; frames are never PMM-owned.
ktl::ref<vmo> create_device_vmo(vm_paddr_t base, size_t pages, vm_cache_mode mode);

}  // namespace kernel::mm
