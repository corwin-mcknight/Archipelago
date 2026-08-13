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
    // Device frames are pinned for the VMO's lifetime. RAM-backed windows in
    // descriptor coverage get marked; true MMIO usually sits above coverage
    // and lookup simply misses.
    g_page_descriptors.mark_range(base, pages, page_state::WIRED);
    return ktl::make_ref<vmo>(pages, pgr);
}

ktl::ref<vmo> create_wired_vmo(vm_paddr_t base, size_t pages) {
    // Same translation-only pager as a device window, but over ordinary cached RAM the boot
    // classification already wired (boot module bytes today). No descriptor marking: the frames'
    // KERNEL state is what keeps them pinned, and marking here would be the un-unmarkable kind
    // create_device_vmo is known for (todo.md).
    auto pgr = ktl::make_ref<device_pager>(base, vm_cache_mode::CACHED);
    if (pgr.get() == nullptr) { return {}; }
    return ktl::make_ref<vmo>(pages, pgr);
}

}  // namespace kernel::mm
