// Entry point. The kernel hands us a stack pointer and jumps to e_entry; there is no loader, no
// dynamic linker, and no argument block yet. Align the stack to the SysV 16-byte boundary before the
// first call and hand off to C++.

.section .text
.globl _start
.type _start, @function
_start:
    // The kernel hands us our IPC buffer in rdi (base) and rsi (size) -- the same registers a
    // SysV call would use, so they pass straight through as init_main's arguments.
    and $-16, %rsp
    call init_main
    ud2
.size _start, . - _start
