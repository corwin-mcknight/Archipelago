#pragma once

#include <kernel/mm/page.h>

namespace kernel::mm {

// Per-address-space paging state: the root table physical address. The same
// shape on every supported architecture (both are 4-level, 512-entry);
// portable code embeds this struct by value but only the paging code touches
// it.
struct arch_aspace {
    vm_paddr_t root_phys = 0;
};

}  // namespace kernel::mm
