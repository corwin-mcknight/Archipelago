#include <std/string.h>

#include "kernel/config.h"
#include "kernel/log.h"
#include "kernel/mm/page_descriptor.h"
#include "kernel/mm/region.h"
#include "kernel/mm/vm_aspace.h"
#include "kernel/mm/vmo.h"

extern uintptr_t g_hhdm_offset;

namespace kernel::mm {

namespace {
constexpr size_t PAGE_SIZE = KERNEL_MINIMUM_PAGE_SIZE;

// Install a private copy of the currently mapped (shared) frame: used when a
// write hits the zero page or a share-counted frame. The fresh frame comes
// from the VMO's pager; contents are copied unless the source is the zero
// page (a zeroed frame already is the copy).
bool resolve_cow(vm_aspace& aspace, region_child& binding, vmo& obj, uint64_t page, uintptr_t page_vaddr) {
    auto old_walk = aspace.arch().walk(page_vaddr);
    if (!old_walk.has_value()) { return false; }
    vm_paddr_t old_frame = old_walk.value() & ~(PAGE_SIZE - 1);

    auto fill            = obj.get_or_fill_page(page);
    if (fill.is_err()) { return false; }  // OOM: unresolvable, fall to crash path
    vm_paddr_t new_frame = fill.unwrap();

    if (old_frame != vmm_zero_page() && old_frame != new_frame) {
        memcpy(reinterpret_cast<void*>(new_frame + g_hhdm_offset), reinterpret_cast<void*>(old_frame + g_hhdm_offset),
               PAGE_SIZE);
        if (page_descriptor* desc = g_page_descriptors.lookup(old_frame)) {
            if (desc->share_count > 0) { --desc->share_count; }
        }
    }

    (void)aspace.arch().unmap_page(page_vaddr);  // also flushes the stale TLB entry
    return aspace.arch().map_page(page_vaddr, new_frame, binding.prot, binding.cache);
}
}  // namespace

bool vmm_handle_fault(const vm_fault& fault) {
    // Only the kernel aspace exists until tasks arrive.
    vm_aspace& aspace = kernel_aspace();
    if (!aspace.has_root()) { return false; }

    kernel::synchronization::lock_guard guard(g_vmm_lock);
    aspace.count_fault();

    region_child* binding = aspace.root().find_binding(fault.vaddr);
    if (binding == nullptr || binding->vmo_ref.get() == nullptr) { return false; }

    // Authorization: the access must fit the binding's protection. CoW is a
    // permission *upgrade path*, not a bypass -- a write needs WRITE here.
    vm_prot_t needed = vm_prot::READ | (fault.write ? vm_prot::WRITE : 0) | (fault.user ? vm_prot::USER : 0);
    if ((needed & ~binding->prot) != 0) { return false; }

    vmo& obj             = *binding->vmo_ref;
    uint64_t page        = (fault.vaddr - binding->base + binding->vmo_offset) / PAGE_SIZE;
    uintptr_t page_vaddr = binding->base + (page - binding->vmo_offset / PAGE_SIZE) * PAGE_SIZE;

    // A binding can outlive a shrink; classify the stale access so the crash
    // dump reads as an out-of-range VMO access, not a wild pointer.
    if (page >= obj.size_pages()) {
        g_log.error("vmm: fault at 0x{0:p}: out-of-range VMO access (page {1} >= size {2})", fault.vaddr, page,
                    obj.size_pages());
        return false;
    }

    if (fault.present) {
        // Translation exists but the access faulted: the CoW case is a write
        // through a read-only PTE where the binding allows writing. Anything
        // else is a genuine violation.
        if (!fault.write) { return false; }
        return resolve_cow(aspace, *binding, obj, page, page_vaddr);
    }

    // No translation. A read of an unpopulated anonymous page shares the
    // global zero page read-only; the first write lands in resolve_cow.
    if (!fault.write && !obj.resident_frame(page).has_value() && obj.backing_pager().owns_frames()) {
        return aspace.arch().map_page(page_vaddr, vmm_zero_page(), binding->prot & ~vm_prot::WRITE, binding->cache);
    }

    // Write, or a page the pager already backs: install the real frame. A
    // frame still share-counted from CoW stays read-only until written.
    auto fill = obj.get_or_fill_page(page);
    if (fill.is_err()) { return false; }
    vm_prot_t prot        = binding->prot;
    page_descriptor* desc = g_page_descriptors.lookup(fill.unwrap());
    if (desc != nullptr && desc->share_count > 1) { prot &= ~vm_prot::WRITE; }
    return aspace.arch().map_page(page_vaddr, fill.unwrap(), prot, binding->cache);
}

}  // namespace kernel::mm
