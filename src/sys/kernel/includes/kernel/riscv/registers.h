#pragma once

#include <stdint.h>

// riscv64 supervisor trap frame. Field order matches the save layout in
// riscv64/trap_entry.S exactly -- the assembly stores by fixed byte offset, so
// any change here must be mirrored there. sp is the pre-trap stack pointer.
typedef struct register_frame {
    uint64_t ra, sp, gp, tp;
    uint64_t t0, t1, t2;
    uint64_t s0, s1;
    uint64_t a0, a1, a2, a3, a4, a5, a6, a7;
    uint64_t s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    uint64_t t3, t4, t5, t6;
    uint64_t sepc, sstatus, scause, stval;
} __attribute__((__packed__)) register_frame_t;
