#pragma once

#include <stddef.h>
#include <stdint.h>

namespace kernel::mm {

typedef uintptr_t vm_paddr_t;

// A physical address at an interface that crosses into direct-map or MMIO access. vm_paddr_t remains
// the PMM/VMM's legacy arithmetic representation for now; conversion to this type is deliberately
// explicit so an unlabelled virtual address cannot reach those access boundaries by accident.
class physical_address {
   public:
    explicit constexpr physical_address(uintptr_t value) : m_value(value) {}

    constexpr uintptr_t value() const { return m_value; }

   private:
    uintptr_t m_value;
};

struct physical_range {
    physical_address base;
    size_t size;
};

struct vm_page_region {
    vm_paddr_t start;
    size_t count;
    // Top `zeroed_count` pages of the region are already zeroed in place.
    // Regions are consumed from the tail, so these are the next pages popped.
    size_t zeroed_count = 0;
};

}  // namespace kernel::mm
