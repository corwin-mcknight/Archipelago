#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kernel::mm {

/// Represents the state of a physical page frame (stored in 4 bits).
enum class vm_page_state : uint8_t {
    FREE   = 0b00,
    ACTIVE = 0b01,
    ZEROED = 0b10,
};

// TODO: Add more page sizes as needed. Kernel assumes 4K pages for now.
enum class vm_page_size : uint8_t {
    SIZE_4K = 0,
};

typedef uintptr_t vm_paddr_t;
typedef uintptr_t vm_page_id_t;

struct vm_page_region {
    vm_paddr_t start;
    size_t count;
};

}  // namespace kernel::mm
