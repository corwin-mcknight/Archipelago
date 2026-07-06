#pragma once

#include <kernel/mm/page.h>

namespace kernel::mm {

// x86_64 shape of the per-address-space paging state: the PML4 root. Portable
// code embeds this struct by value but never reads its fields; the vm_aspace
// methods that touch it live in x86_64/paging.cpp.
struct arch_aspace {
    vm_paddr_t pml4_phys = 0;
};

}  // namespace kernel::mm
