#include "kernel/mm/vmo.h"

#include "kernel/assert.h"
#include "kernel/config.h"
#include "kernel/mm/page_descriptor.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/region.h"
#include "kernel/mm/vm_aspace.h"

extern uintptr_t g_hhdm_offset;

namespace kernel::mm {

namespace { constexpr size_t PAGE_SIZE = KERNEL_MINIMUM_PAGE_SIZE; }

vmo::vmo(size_t pages, ktl::ref<pager> pgr) : obj::Object(TYPE_ID), m_pages(pages), m_pager(ktl::move(pgr)) {
    size_t chunk_count = (pages + CHUNK_ENTRIES - 1) / CHUNK_ENTRIES;
    for (size_t i = 0; i < chunk_count; ++i) { (void)m_chunks.push_back(nullptr); }
}

vmo::~vmo() {
    // Bindings hold a ref, so a dying VMO has no mappings left to zap.
    assert(m_mappings.empty(), "vmo destroyed while still mapped");
    for (size_t i = 0; i < m_chunks.size(); ++i) {
        uint64_t* chunk = m_chunks[i];
        if (chunk == nullptr) { continue; }
        for (size_t e = 0; e < CHUNK_ENTRIES; ++e) {
            if (chunk[e] == 0) { continue; }
            if (page_descriptor* desc = g_page_descriptors.lookup(chunk[e])) {
                desc->owner       = nullptr;
                desc->offset      = 0;
                desc->share_count = 0;
            }
            // Device windows keep their frames; only pager-owned frames
            // return to the PMM.
            if (m_pager->owns_frames()) { g_page_frame_allocator.free(chunk[e]); }
        }
        g_page_frame_allocator.free(reinterpret_cast<uintptr_t>(chunk) - g_hhdm_offset);
    }
}

uint64_t* vmo::chunk_for(uint64_t page, bool allocate) {
    size_t index = page / CHUNK_ENTRIES;
    if (index >= m_chunks.size()) { return nullptr; }
    if (m_chunks[index] == nullptr && allocate) {
        // Chunk frames arrive zeroed from the PMM: all entries start absent.
        auto frame = g_page_frame_allocator.alloc();
        if (!frame.has_value()) { return nullptr; }
        m_chunks[index] = reinterpret_cast<uint64_t*>(frame.value() + g_hhdm_offset);
    }
    return m_chunks[index];
}

ktl::maybe<vm_paddr_t> vmo::resident_frame(uint64_t page) const {
    if (page >= m_pages) { return ktl::nothing; }
    size_t index = page / CHUNK_ENTRIES;
    if (index >= m_chunks.size() || m_chunks[index] == nullptr) { return ktl::nothing; }
    uint64_t entry = m_chunks[index][page % CHUNK_ENTRIES];
    if (entry == 0) { return ktl::nothing; }
    return entry;
}

ktl::result<void> vmo::fill_page(uint64_t page) {
    uint64_t* chunk = chunk_for(page, /*allocate=*/true);
    if (chunk == nullptr) { return ktl::err(ktl::errc::oom); }
    uint64_t& entry = chunk[page % CHUNK_ENTRIES];
    if (entry != 0) { return ktl::result<void>::ok(); }  // already resident

    auto frame = m_pager->fill(page);
    if (frame.is_err()) { return ktl::err(frame.unwrap_err()); }
    entry = frame.unwrap();

    if (page_descriptor* desc = g_page_descriptors.lookup(entry)) {
        desc->owner  = this;
        desc->offset = page;
    }
    ++m_resident;
    ++m_fills;
    return ktl::result<void>::ok();
}

ktl::result<vm_paddr_t> vmo::get_or_fill_page(uint64_t page) {
    if (page >= m_pages) { return ktl::err(ktl::errc::out_of_range); }
    auto res = fill_page(page);
    if (res.is_err()) { return ktl::err(res.unwrap_err()); }
    return ktl::result<vm_paddr_t>::ok(resident_frame(page).value());
}

ktl::result<void> vmo::commit(uint64_t page, size_t count) {
    if (page + count < page || page + count > m_pages) { return ktl::err(ktl::errc::out_of_range); }
    kernel::synchronization::critical_irq_lock_guard guard(g_vmm_lock);
    for (uint64_t p = page; p < page + count; ++p) {
        auto res = fill_page(p);
        if (res.is_err()) { return res; }
    }
    return ktl::result<void>::ok();
}

void vmo::zap_page_mappings(uint64_t page) {
    for (size_t i = 0; i < m_mappings.size(); ++i) {
        const mapping& m    = m_mappings[i];
        uint64_t first_page = m.binding->vmo_offset / PAGE_SIZE;
        uint64_t page_span  = m.binding->size / PAGE_SIZE;
        if (page < first_page || page >= first_page + page_span) { continue; }
        uintptr_t vaddr = m.binding->base + (page - first_page) * PAGE_SIZE;
        (void)m.aspace->unmap_page(vaddr);
    }
}

void vmo::decommit_page(uint64_t page) {
    size_t index = page / CHUNK_ENTRIES;
    if (index >= m_chunks.size() || m_chunks[index] == nullptr) { return; }
    uint64_t& entry = m_chunks[index][page % CHUNK_ENTRIES];
    if (entry == 0) { return; }

    zap_page_mappings(page);

    if (page_descriptor* desc = g_page_descriptors.lookup(entry)) {
        desc->owner       = nullptr;
        desc->offset      = 0;
        desc->share_count = 0;
    }
    if (m_pager->owns_frames()) { g_page_frame_allocator.free(entry); }
    entry = 0;
    --m_resident;
}

ktl::result<void> vmo::set_size(size_t new_pages) {
    kernel::synchronization::critical_irq_lock_guard guard(g_vmm_lock);
    if (!m_pager->resizable()) { return ktl::err(ktl::errc::invalid_operation); }

    if (new_pages < m_pages) {
        for (uint64_t p = new_pages; p < m_pages; ++p) {
            // Zap unconditionally: a non-resident page may still carry a
            // shared zero-page translation, and stale access must fault.
            zap_page_mappings(p);
            decommit_page(p);
        }
        size_t needed_chunks = (new_pages + CHUNK_ENTRIES - 1) / CHUNK_ENTRIES;
        while (m_chunks.size() > needed_chunks) {
            uint64_t* chunk = m_chunks[m_chunks.size() - 1];
            if (chunk != nullptr) { g_page_frame_allocator.free(reinterpret_cast<uintptr_t>(chunk) - g_hhdm_offset); }
            (void)m_chunks.pop_back();
        }
    } else {
        size_t needed_chunks = (new_pages + CHUNK_ENTRIES - 1) / CHUNK_ENTRIES;
        while (m_chunks.size() < needed_chunks) {
            if (!m_chunks.push_back(nullptr)) { return ktl::err(ktl::errc::oom); }
        }
    }
    m_pages = new_pages;
    return ktl::result<void>::ok();
}

ktl::result<void> vmo::decommit(uint64_t page, size_t count) {
    if (page + count < page || page + count > m_pages) { return ktl::err(ktl::errc::out_of_range); }
    kernel::synchronization::critical_irq_lock_guard guard(g_vmm_lock);
    for (uint64_t p = page; p < page + count; ++p) { decommit_page(p); }
    return ktl::result<void>::ok();
}

void vmo::add_mapping(vm_aspace& aspace, region_child& binding) {
    (void)m_mappings.push_back({.aspace = &aspace, .binding = &binding});
}

void vmo::remove_mapping(region_child& binding) {
    for (size_t i = 0; i < m_mappings.size(); ++i) {
        if (m_mappings[i].binding == &binding) {
            m_mappings[i] = m_mappings[m_mappings.size() - 1];
            (void)m_mappings.pop_back();
            return;
        }
    }
}

// All anonymous VMOs share one stateless zero-fill pager. Global-scope so the
// initializer runs from the global-ctor pass (no __cxa_guard in the kernel).
namespace { ktl::ref<pager> g_anonymous_pager = ktl::make_ref<anonymous_pager>(); }  // namespace

ktl::ref<vmo> create_anonymous_vmo(size_t pages) { return ktl::make_ref<vmo>(pages, g_anonymous_pager); }

}  // namespace kernel::mm
