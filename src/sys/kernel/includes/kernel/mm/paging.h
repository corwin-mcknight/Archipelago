#pragma once

#include <stddef.h>
#include <stdint.h>

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

}  // namespace kernel::mm
