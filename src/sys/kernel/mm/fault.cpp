#include "kernel/assert.h"
#include "kernel/config.h"
#include "kernel/log.h"
#include "kernel/mm/region.h"
#include "kernel/mm/vm_aspace.h"
#include "kernel/mm/vmo.h"

namespace kernel::mm {

namespace {
constexpr size_t PAGE_SIZE = KERNEL_MINIMUM_PAGE_SIZE;

// Break zero-page sharing: back the page with a fresh frame from the pager
// and remap writable. Until VMO clone lands, the zero page is the only frame
// that can be CoW-shared, so no contents need copying -- a fresh anonymous
// frame is already zeroed. Frame-copying CoW arrives with VMO clone.
bool resolve_cow(vm_aspace& aspace, region_child& binding, vmo& obj, uint64_t page, uintptr_t page_vaddr) {
    auto old_walk = aspace.walk(page_vaddr);
    if (!old_walk.has_value()) { return false; }
    assert((old_walk.value() & ~(PAGE_SIZE - 1)) == vmm_zero_page(),
           "CoW write on a non-zero-page frame before VMO clone exists");

    auto fill = obj.get_or_fill_page(page);
    if (fill.is_err()) { return false; }  // OOM: unresolvable, fall to crash path

    (void)aspace.unmap_page(page_vaddr);  // also flushes the stale TLB entry
    return aspace.map_page(page_vaddr, fill.unwrap(), binding.prot, binding.cache);
}
}  // namespace

bool vmm_handle_fault(const vm_fault& fault) {
    // Resolve against whichever space is live on this CPU -- the kernel
    // aspace normally, a scratch/task space when one is activated.
    vm_aspace* active = vm_aspace::active();
    if (active == nullptr || !active->has_root()) { return false; }
    vm_aspace& aspace = *active;

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
    uintptr_t page_vaddr = fault.vaddr & ~(PAGE_SIZE - 1);

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
        return aspace.map_page(page_vaddr, vmm_zero_page(), binding->prot & ~vm_prot::WRITE, binding->cache);
    }

    // Write, or a page the pager already backs: install the real frame.
    auto fill = obj.get_or_fill_page(page);
    if (fill.is_err()) { return false; }
    return aspace.map_page(page_vaddr, fill.unwrap(), binding->prot, binding->cache);
}

}  // namespace kernel::mm
