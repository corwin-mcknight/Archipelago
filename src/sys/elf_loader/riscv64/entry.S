.section .text
.globl loader_worker_entry
.type loader_worker_entry, @function
loader_worker_entry:
    ld a2, 0(sp)
    call loader_worker
    unimp
.size loader_worker_entry, . - loader_worker_entry
