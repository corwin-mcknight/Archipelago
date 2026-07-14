#pragma once

#include <kernel/config.h>
#include <stddef.h>
#include <stdint.h>

namespace kernel::synchronization::lockdep {

uint32_t allocate_identity(const void* address, const char* name);
void release_identity(uint32_t identity);
void acquired(const void* address, uint32_t identity, const char* file, uint32_t line);
void released(const void* address, uint32_t identity);
void assert_not_owned(const void* address, uint32_t identity);

#if CONFIG_KERNEL_TESTING
void reset_for_testing();
size_t edge_count_for_testing();
#endif

}  // namespace kernel::synchronization::lockdep
