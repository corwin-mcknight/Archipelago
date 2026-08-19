#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kernel::mm {

typedef uintptr_t vm_paddr_t;

struct vm_page_region {
    vm_paddr_t start;
    size_t count;
    // Top `zeroed_count` pages of the region are already zeroed in place.
    // Regions are consumed from the tail, so these are the next pages popped.
    size_t zeroed_count = 0;
};

}  // namespace kernel::mm
