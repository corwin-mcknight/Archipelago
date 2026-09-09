#include "kernel/config.h"
#include "kernel/mm/page_descriptor.h"
#include "kernel/mm/pager.h"
#include "kernel/mm/vmo.h"

namespace kernel::mm {

// Pure address translation: the window's frames exist before the VMO does,
// so fill never allocates.
ktl::result<vm_paddr_t> device_pager::fill(uint64_t page) {
    return ktl::result<vm_paddr_t>::ok(m_base + page * KERNEL_MINIMUM_PAGE_SIZE);
}

ktl::ref<vmo> create_device_vmo(vm_paddr_t base, size_t pages, vm_cache_mode mode) {
    auto pgr = ktl::make_ref<device_pager>(base, mode);
    if (pgr.get() == nullptr) { return {}; }
    auto object = vmo::create(pages, ktl::move(pgr));
    if (!object) { return {}; }
    // Defer WIRED marking until construction succeeds so failure leaves the
    // window unchanged. Uncovered frames are ignored; marks on covered frames
    // currently survive VMO destruction; reservation ownership is unresolved.
    g_page_descriptors.mark_range(base, pages, page_state::WIRED);
    return object;
}

ktl::ref<vmo> create_wired_vmo(vm_paddr_t base, size_t pages) {
    // The caller has already reserved this RAM. Boot module frames, for example,
    // arrive with WIRED descriptors; preserve that existing reservation.
    auto pgr = ktl::make_ref<device_pager>(base, vm_cache_mode::CACHED);
    if (pgr.get() == nullptr) { return {}; }
    return vmo::create(pages, ktl::move(pgr));
}

}  // namespace kernel::mm
