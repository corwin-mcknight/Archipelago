#include "kernel/mm/vm_aspace.h"

#include <kernel/obj/type_registry.h>

#include "kernel/log.h"
#include "kernel/mm/page_descriptor.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmo.h"
#include "kernel/panic.h"

namespace kernel::mm {

kernel::synchronization::spinlock g_vmm_lock;

namespace {
vm_aspace g_kernel_aspace;
vm_paddr_t g_zero_page = 0;
}  // namespace

vm_aspace& kernel_aspace() { return g_kernel_aspace; }

vm_paddr_t vmm_zero_page() { return g_zero_page; }

namespace {
// Root regions span the low canonical half minus the null page. Kernel-half
// virtual addresses stay outside the region tree -- the boot mappings (HHDM,
// kernel image) are not region-managed.
constexpr uintptr_t ROOT_BASE = 0x1000;
constexpr vm_prot_t ROOT_PROT = vm_prot::READ | vm_prot::WRITE | vm_prot::EXECUTE | vm_prot::USER;

ktl::ref<Region> make_root(vm_aspace& aspace) {
    return ktl::make_ref<Region>(aspace, ROOT_BASE, vm_aspace::low_limit() - ROOT_BASE, ROOT_PROT);
}
}  // namespace

vm_aspace::~vm_aspace() { destroy(); }

bool vm_aspace::init() {
    if (!arch_init()) { return false; }
    m_root = make_root(*this);
    return m_root.get() != nullptr;
}

void vm_aspace::destroy() {
    // Regions first: their teardown zaps translations through the arch
    // tables, which must still be alive.
    m_root = ktl::ref<Region>{};
    arch_destroy();
}

void vmm_init(const vm_page_region* usable, size_t usable_count, const vm_page_region* wired, size_t wired_count) {
    Region::register_type(obj::g_type_registry).expect("vmm: Region type registration failed");
    vmo::register_type(obj::g_type_registry).expect("vmm: VMO type registration failed");

    if (!g_page_descriptors.init(usable, usable_count, wired, wired_count)) {
        panic("vmm: page descriptor array allocation failed");
    }
    // The kernel builds its own page tables (deep copy of the boot kernel
    // half) and switches onto them -- the bootloader's tables sit in
    // reclaimable memory the PMM hands out, so running on them is unsafe.
    if (!g_kernel_aspace.arch_init_kernel()) { panic("vmm: kernel page table clone failed"); }
    g_kernel_aspace.activate();
    g_kernel_aspace.m_root = make_root(g_kernel_aspace);
    if (g_kernel_aspace.m_root.get() == nullptr) { panic("vmm: kernel root region allocation failed"); }

    // The global zero page: wired for the kernel's lifetime, permanently
    // share-counted so the CoW path never tries to reclaim it.
    g_zero_page = g_page_frame_allocator.alloc().expect("vmm: zero page allocation failed");
    if (page_descriptor* desc = g_page_descriptors.lookup(g_zero_page)) {
        desc->state       = page_state::WIRED;
        desc->share_count = 1;
    }
    g_log.info("vmm: kernel running on its own page tables");
}

}  // namespace kernel::mm
