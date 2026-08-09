// Entry point. The kernel hands us a stack pointer and jumps to e_entry; there is no loader, no
// dynamic linker, and no argument block yet.

.section .text
.globl _start
.type _start, @function
_start:
    // The kernel hands us our IPC buffer in a0 (base) and a1 (size) -- the same registers a call
    // would use, so they pass straight through as __crt_start's arguments.
    call __crt_start
    unimp
.size _start, . - _start
