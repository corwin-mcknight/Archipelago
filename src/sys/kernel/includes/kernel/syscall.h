#pragma once

#include <stdint.h>

namespace kernel::syscall {

constexpr uint64_t SYS_EXIT       = 0;
constexpr uint64_t SYS_YIELD      = 1;
constexpr uint64_t SYS_SLEEP      = 2;  // arg0 = kernel ticks
constexpr uint64_t SYS_DEBUG_PUTC = 3;  // arg0 = one character, line-buffered into the kernel log

}  // namespace kernel::syscall

extern "C" uint64_t syscall_dispatch(uint64_t nr, uint64_t arg0);
