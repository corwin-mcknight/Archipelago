#pragma once

#include <abi/syscall.h>
#include <stdint.h>

// The syscall numbers themselves are the user/kernel contract and live in <abi/syscall.h>, which
// user programs compile against. The alias keeps kernel code spelling them kernel::syscall::SYS_*.
namespace kernel {
namespace syscall = ::abi::syscall;
}

// Six argument registers -- as many as either architecture's calling convention carries, so the
// entry assembly never needs widening. Two is all any syscall reads today; the rest are
// carried for the handle-dispatch pipeline, which will prepend a handle to every operation.
//
// Note this is a seven-parameter function, one past what SysV passes in registers on x86_64, so a5
// arrives on the stack there. riscv64 has eight argument registers and passes all seven.
extern "C" uint64_t syscall_dispatch(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                                     uint64_t a5);
