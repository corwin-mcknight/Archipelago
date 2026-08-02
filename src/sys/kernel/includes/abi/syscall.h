#pragma once

#include <stdint.h>

// The user/kernel syscall contract. Public: installed to /usr/include by sys/kernel-headers and
// compiled into both the kernel and every user program, so the two cannot drift. Nothing
// kernel-internal belongs here -- see <kernel/syscall.h> for the dispatch side.

namespace abi::syscall {

// The syscall number names the operation; a handle argument names the object it acts on.
constexpr uint64_t SYS_EXIT             = 0;
constexpr uint64_t SYS_YIELD            = 1;
constexpr uint64_t SYS_SLEEP            = 2;  // arg0 = kernel ticks
// arg0 = offset into this thread's IPC buffer, arg1 = byte count. Returns bytes written, or a
// negative ktl::errc. No pointer crosses the boundary: the buffer is the only memory a syscall
// reads from the caller, and the kernel already knows where its pages are.
constexpr uint64_t SYS_WRITE            = 3;

// Handle operations. Every one of them takes a handle in arg0 and runs the same verification
// pipeline before any operation code executes: slot-and-generation lookup in the calling task's
// handle table, then a type check, then a rights check. A failure at any step returns a negative
// error with nothing done.
//
// A handle is a uint64: table slot index in the low 32 bits, slot generation in the high 32.
// Reusing a closed handle fails the generation check -- a recycled slot has a new generation.
//
// Error returns occupy the small negative band (roughly -1 .. -64). A successful
// SYS_HANDLE_DUPLICATE returns the new handle, which cannot fall in that band: it would take a
// slot index above 0xFFFFFFC0 -- four billion live handles -- for a handle to look negative.
constexpr uint64_t SYS_HANDLE_CLOSE     = 4;  // arg0 = handle. Returns 0.
constexpr uint64_t SYS_HANDLE_DUPLICATE = 5;  // arg0 = handle (needs the duplicate right),
                                              // arg1 = rights mask. Returns the new handle.
constexpr uint64_t SYS_OBJ_INFO         = 6;  // arg0 = handle. Returns type id in the low 32 bits,
                                              // the handle's rights in the high 32.

// The initial thread's handle table is created with exactly two entries, in this order: a handle
// to its own task, then a handle to its own thread. Both are first-generation, so their packed
// values are the slot indices themselves.
constexpr uint64_t SELF_TASK_HANDLE     = 0;
constexpr uint64_t SELF_THREAD_HANDLE   = 1;

}  // namespace abi::syscall
