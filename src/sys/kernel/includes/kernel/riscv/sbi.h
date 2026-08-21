#pragma once

#include <stdint.h>

// SBI calls the kernel makes beyond the timer (which keeps its own in riscv64/timer.cpp).
namespace kernel::riscv::sbi {

struct ret {
    int64_t error;
    int64_t value;
};

inline ret call(uint64_t ext, uint64_t fid, uint64_t arg0 = 0, uint64_t arg1 = 0, uint64_t arg2 = 0,
                uint64_t arg3 = 0) {
    register uint64_t a0 asm("a0") = arg0;
    register uint64_t a1 asm("a1") = arg1;
    register uint64_t a2 asm("a2") = arg2;
    register uint64_t a3 asm("a3") = arg3;
    register uint64_t a6 asm("a6") = fid;
    register uint64_t a7 asm("a7") = ext;
    asm volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a2), "r"(a3), "r"(a6), "r"(a7) : "memory");
    return {static_cast<int64_t>(a0), static_cast<int64_t>(a1)};
}

// hart_mask bit n names hart (hart_mask_base + n); the kernel always passes base 0.
inline ret send_ipi(uint64_t hart_mask) { return call(0x735049, 0, hart_mask, 0); }
inline ret remote_sfence_vma(uint64_t hart_mask, uintptr_t start, uintptr_t size) {
    return call(0x52464E43, 1, hart_mask, 0, start, size);
}

}  // namespace kernel::riscv::sbi
