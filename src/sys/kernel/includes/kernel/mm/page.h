#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kernel::mm {

// TODO: Add more page sizes as needed. Kernel assumes 4K pages for now.
enum class vm_page_size : uint8_t {
    SIZE_4K = 0,
};

typedef uintptr_t vm_paddr_t;
typedef uintptr_t vm_page_id_t;

struct vm_page_region {
    vm_paddr_t start;
    size_t count;
    // Top `zeroed_count` pages of the region are already zeroed in place.
    // Regions are consumed from the tail, so these are the next pages popped.
    size_t zeroed_count = 0;
};

}  // namespace kernel::mm
