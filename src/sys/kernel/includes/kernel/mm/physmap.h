#pragma once

#include <kernel/mm/page.h>
#include <stddef.h>
#include <stdint.h>

namespace kernel::mm {

void direct_map_initialize(uintptr_t base);
bool direct_map_ready();

// Migration seam for MM implementation code that genuinely needs pointer identity. Prefer the
// bounded frame operations below in clients that only need to copy or clear bytes.
uintptr_t direct_map_address(physical_address address);
physical_address direct_map_physical(const void* address);

void zero_frame(vm_paddr_t frame);
void copy_to_frame(vm_paddr_t frame, size_t offset, const void* source, size_t length);
void copy_from_frame(void* destination, vm_paddr_t frame, size_t offset, size_t length);

namespace unsafe {

// Deliberate arbitrary physical access for crash recovery and trusted diagnostic facilities.
uintptr_t direct_map_address(physical_address address);
physical_address direct_map_physical(const void* address);

}  // namespace unsafe

}  // namespace kernel::mm
