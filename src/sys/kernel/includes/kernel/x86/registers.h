#pragma once

#include <stdint.h>

#ifndef ARCH_X86
typedef struct register_frame {
    unsigned int gs, fs, es, ds;
    unsigned int edi, esi, ebp, ebx, edx, ecx, eax;
    unsigned int int_no, err_code;
    unsigned int eip, cs, eflags, esp, ss;
} __attribute__((__packed__)) register_frame_t;

#else

typedef struct register_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, eflags, userrsp, ss;
} __attribute__((__packed__)) register_frame_t;

#endif