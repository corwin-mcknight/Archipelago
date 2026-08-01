#pragma once

#include <stdint.h>

// The user/kernel syscall contract. Public: installed to /usr/include by sys/kernel-headers and
// compiled into both the kernel and every user program, so the two cannot drift. Nothing
// kernel-internal belongs here -- see <kernel/syscall.h> for the dispatch side.

namespace abi::syscall {

// The syscall number names the operation. Operations will additionally name the object they act on
// through a handle argument once the dispatch pipeline exists; the number stays the opcode.
constexpr uint64_t SYS_EXIT  = 0;
constexpr uint64_t SYS_YIELD = 1;
constexpr uint64_t SYS_SLEEP = 2;  // arg0 = kernel ticks
// arg0 = offset into this thread's IPC buffer, arg1 = byte count. Returns bytes written, or a
// negative ktl::errc. No pointer crosses the boundary: the buffer is the only memory a syscall
// reads from the caller, and the kernel already knows where its pages are.
constexpr uint64_t SYS_WRITE = 3;

}  // namespace abi::syscall
