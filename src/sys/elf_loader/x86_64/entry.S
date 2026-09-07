.section .text
.globl loader_worker_entry
.type loader_worker_entry, @function
loader_worker_entry:
    mov (%rsp), %rdx
    call loader_worker
    ud2
.size loader_worker_entry, . - loader_worker_entry
