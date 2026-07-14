#include "kernel/mm/region.h"

#include "kernel/config.h"
#include "kernel/mm/vm_aspace.h"
#include "kernel/mm/vmo.h"

namespace kernel::mm {

namespace {
constexpr size_t PAGE_SIZE = KERNEL_MINIMUM_PAGE_SIZE;

bool page_aligned(uintptr_t value) { return (value & (PAGE_SIZE - 1)) == 0; }

// prot is a subset of limit: no bit set that limit lacks.
bool prot_within(vm_prot_t prot, vm_prot_t limit) { return (prot & ~limit) == 0; }

// Cache modes may only degrade toward stricter: DEVICE beats WRITE_COMBINING
// beats CACHED. A binding never maps a pager's frames more cacheable than the
// pager requires.
vm_cache_mode stricter_cache(vm_cache_mode a, vm_cache_mode b) {
    if (a == vm_cache_mode::DEVICE || b == vm_cache_mode::DEVICE) { return vm_cache_mode::DEVICE; }
    if (a == vm_cache_mode::WRITE_COMBINING || b == vm_cache_mode::WRITE_COMBINING) {
        return vm_cache_mode::WRITE_COMBINING;
    }
    return vm_cache_mode::CACHED;
}
}  // namespace

Region::Region(vm_aspace& aspace, uintptr_t base, size_t size, vm_prot_t max_prot)
    : obj::Object(TYPE_ID), m_aspace(aspace), m_base(base), m_size(size), m_max_prot(max_prot) {}

Region::~Region() { clear_children(); }

void Region::clear_children() {
    while (!m_children.empty()) { remove_slot(*m_children.begin()); }
}

ktl::result<region_child*> Region::insert_slot(uintptr_t base, size_t size, vm_prot_t prot) {
    if (size == 0 || !page_aligned(base) || !page_aligned(size)) { return ktl::err(ktl::errc::invalid_operation); }
    uintptr_t end = base + size;
    if (end < base) { return ktl::err(ktl::errc::out_of_range); }  // wrap
    if (base < m_base || end > m_base + m_size) { return ktl::err(ktl::errc::out_of_range); }
    if (!prot_within(prot, m_max_prot)) { return ktl::err(ktl::errc::rights_violation); }

    // Siblings are disjoint, so the only slot that could overlap [base, end)
    // is the last one starting before end.
    auto it = m_children.find_le(end - 1);
    if (it != m_children.end() && it->base + it->size > base) { return ktl::err(ktl::errc::invalid_operation); }

    auto* slot = new region_child{};
    if (slot == nullptr) { return ktl::err(ktl::errc::oom); }
    slot->base = base;
    slot->size = size;
    slot->prot = prot;
    m_children.insert(*slot);
    return ktl::result<region_child*>::ok(slot);
}

void Region::remove_slot(region_child& slot) {
    if (slot.is_binding()) {
        zap_range(slot.base, slot.size);
        if (slot.vmo_ref.get() != nullptr) { slot.vmo_ref->remove_mapping(slot); }
    } else {
        slot.child->clear_children();
    }
    m_children.erase(slot);
    delete &slot;  // drops the child-region or VMO ref
}

void Region::zap_range(uintptr_t base, size_t size) {
    for (uintptr_t vaddr = base; vaddr < base + size; vaddr += PAGE_SIZE) {
        (void)m_aspace.unmap_page(vaddr);  // absent pages are fine
    }
}

ktl::result<ktl::ref<Region>> Region::create_child(uintptr_t base, size_t size, vm_prot_t max_prot) {
    kernel::synchronization::critical_irq_lock_guard guard(g_vmm_lock);
    auto slot = insert_slot(base, size, max_prot);
    if (slot.is_err()) { return ktl::err(slot.unwrap_err()); }

    auto child = ktl::make_ref<Region>(m_aspace, base, size, max_prot);
    if (child.get() == nullptr) {
        remove_slot(*slot.unwrap());
        return ktl::err(ktl::errc::oom);
    }
    slot.unwrap()->child = child;
    return ktl::result<ktl::ref<Region>>::ok(child);
}

ktl::result<void> Region::map(uintptr_t vaddr, size_t size, ktl::ref<vmo> vmo_ref, uint64_t vmo_offset, vm_prot_t prot,
                              vm_cache_mode cache) {
    kernel::synchronization::critical_irq_lock_guard guard(g_vmm_lock);
    if (vmo_ref.get() != nullptr) {
        if (!page_aligned(vmo_offset)) { return ktl::err(ktl::errc::invalid_operation); }
        uint64_t end_page = (vmo_offset + size) / PAGE_SIZE;
        if (vmo_offset + size < vmo_offset || end_page > vmo_ref->size_pages()) {
            return ktl::err(ktl::errc::out_of_range);
        }
    }
    auto slot = insert_slot(vaddr, size, prot);
    if (slot.is_err()) { return ktl::err(slot.unwrap_err()); }

    region_child* s = slot.unwrap();
    s->vmo_ref      = ktl::move(vmo_ref);
    s->vmo_offset   = vmo_offset;
    s->cache        = cache;
    if (s->vmo_ref.get() != nullptr) {
        s->cache = stricter_cache(cache, s->vmo_ref->backing_pager().cache_mode());
        s->vmo_ref->add_mapping(m_aspace, *s);
    }
    return ktl::result<void>::ok();
}

ktl::result<void> Region::unmap(uintptr_t base, size_t size) {
    kernel::synchronization::critical_irq_lock_guard guard(g_vmm_lock);
    uintptr_t end = base + size;
    if (end < base || base < m_base || end > m_base + m_size) { return ktl::err(ktl::errc::out_of_range); }

    // Reject a range that cuts through a slot before removing anything, so a
    // failed unmap changes nothing.
    for (auto it = m_children.begin(); it != m_children.end(); ++it) {
        uintptr_t slot_end = it->base + it->size;
        bool overlaps      = it->base < end && slot_end > base;
        bool contained     = it->base >= base && slot_end <= end;
        if (overlaps && !contained) { return ktl::err(ktl::errc::invalid_operation); }
    }

    auto it = m_children.lower_bound(base);
    while (it != m_children.end() && it->base < end) {
        region_child& slot = *it;
        ++it;
        remove_slot(slot);
    }
    return ktl::result<void>::ok();
}

ktl::result<void> Region::protect(uintptr_t base, size_t size, vm_prot_t prot) {
    kernel::synchronization::critical_irq_lock_guard guard(g_vmm_lock);
    uintptr_t end = base + size;
    if (end < base || base < m_base || end > m_base + m_size) { return ktl::err(ktl::errc::out_of_range); }

    // Validate first: narrowing only, bindings only, no partial coverage.
    for (auto it = m_children.lower_bound(base); it != m_children.end() && it->base < end; ++it) {
        if (!it->is_binding()) { return ktl::err(ktl::errc::invalid_operation); }
        if (it->base + it->size > end) { return ktl::err(ktl::errc::invalid_operation); }
        if (!prot_within(prot, it->prot)) { return ktl::err(ktl::errc::rights_violation); }
    }

    for (auto it = m_children.lower_bound(base); it != m_children.end() && it->base < end; ++it) {
        it->prot = prot;
        // Installed translations refill through the fault path with the
        // narrowed protection.
        zap_range(it->base, it->size);
    }
    return ktl::result<void>::ok();
}

region_child* Region::find_binding(uintptr_t vaddr) {
    auto it = m_children.find_le(vaddr);
    if (it == m_children.end() || vaddr >= it->base + it->size) { return nullptr; }
    if (!it->is_binding()) { return it->child->find_binding(vaddr); }
    return &*it;
}

}  // namespace kernel::mm
