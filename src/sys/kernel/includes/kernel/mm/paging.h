#pragma once

#include <stddef.h>
#include <stdint.h>

#include <ktl/maybe>

#include "kernel/mm/page.h"

namespace kernel::mm {

// Arch-neutral protection flags, combined as a bitmask. Each architecture
// translates these to its own page-table entry format internally; portable
// code never reasons about PTE bit positions.
using vm_prot_t = uint32_t;
namespace vm_prot {
constexpr vm_prot_t NONE    = 0;
constexpr vm_prot_t READ    = 1u << 0;
constexpr vm_prot_t WRITE   = 1u << 1;
constexpr vm_prot_t EXECUTE = 1u << 2;
constexpr vm_prot_t USER    = 1u << 3;
}  // namespace vm_prot

// Cache modes are requests, not guarantees: an architecture may degrade a
// mapping toward stricter caching but never looser. On x86_64 DEVICE and
// WRITE_COMBINING both map to uncached (PCD) until PAT programming exists;
// CACHED is write-back.
enum class vm_cache_mode : uint8_t {
    CACHED,
    DEVICE,
    WRITE_COMBINING,
};

// Result of a permission-carrying walk: the physical address a virtual address
// resolves to plus the arch-neutral attributes the mapping was installed with.
struct vm_translation {
    vm_paddr_t paddr;
    vm_prot_t prot;
    vm_cache_mode cache;
};

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

    // Allocate a fresh top-level table and clone the kernel half from the
    // currently active space so the kernel is mapped in every address space.
    bool init();
    void destroy();

    // Wrap the live boot page tables (current CR3) instead of allocating fresh
    // ones, and record this space as active. Used once by the kernel aspace,
    // which is never destroyed -- destroy() on an adopted space would free
    // bootloader-owned tables.
    void adopt_active();

    bool is_valid() const { return m_pml4_phys != 0; }
    vm_paddr_t pml4_phys() const { return m_pml4_phys; }

    // Install a 4K translation with the given protection and cache mode.
    bool map_page(uintptr_t vaddr, vm_paddr_t paddr, vm_prot_t prot, vm_cache_mode cache = vm_cache_mode::CACHED);
    // Resolve a virtual address to its physical address (offset preserved).
    ktl::maybe<vm_paddr_t> walk(uintptr_t vaddr) const;
    // Resolve and report the mapping's protection and cache mode as well.
    ktl::maybe<vm_translation> walk_ext(uintptr_t vaddr) const;
    ktl::maybe<vm_paddr_t> unmap_page(uintptr_t vaddr);

    // Load this space's page tables into the CPU (writes CR3) and record it as
    // the active space. Single-CPU scoped; cross-CPU shootdown is out of scope.
    void activate();
    static const address_space* active();

    // Exclusive end of the low (non-kernel) virtual address half. The value is
    // arch-specific (canonical-form on x86_64, Sv39/Sv48 on riscv64); portable
    // code treats it as an opaque limit.
    static uintptr_t low_limit();

   private:
    vm_paddr_t m_pml4_phys = 0;
};

}  // namespace kernel::mm
