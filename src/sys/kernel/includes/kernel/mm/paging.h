#pragma once

#include <stddef.h>
#include <stdint.h>

#include <ktl/maybe>

#include "kernel/mm/page.h"

namespace kernel::mm {

namespace pte {
constexpr uint64_t PRESENT       = 1ull << 0;
constexpr uint64_t WRITABLE      = 1ull << 1;
constexpr uint64_t USER          = 1ull << 2;
constexpr uint64_t WRITE_THROUGH = 1ull << 3;
constexpr uint64_t CACHE_DISABLE = 1ull << 4;
constexpr uint64_t HUGE          = 1ull << 7;
constexpr uint64_t GLOBAL        = 1ull << 8;
constexpr uint64_t NO_EXECUTE    = 1ull << 63;

constexpr uint64_t ADDR_MASK = 0x000FFFFFFFFFF000ull;
}  // namespace pte

class address_space {
   public:
    address_space() = default;
    ~address_space() { destroy(); }

    address_space(const address_space&)            = delete;
    address_space& operator=(const address_space&) = delete;

    address_space(address_space&& other) noexcept : m_pml4_phys(other.m_pml4_phys) { other.m_pml4_phys = 0; }
    address_space& operator=(address_space&& other) noexcept {
        if (this != &other) {
            destroy();
            m_pml4_phys       = other.m_pml4_phys;
            other.m_pml4_phys = 0;
        }
        return *this;
    }

    bool init();
    void destroy();

    bool is_valid() const { return m_pml4_phys != 0; }
    vm_paddr_t pml4_phys() const { return m_pml4_phys; }

    bool                   map_page(uintptr_t vaddr, vm_paddr_t paddr, uint64_t flags);
    ktl::maybe<vm_paddr_t> walk(uintptr_t vaddr) const;
    ktl::maybe<vm_paddr_t> unmap_page(uintptr_t vaddr);

   private:
    vm_paddr_t m_pml4_phys = 0;
};

}  // namespace kernel::mm
