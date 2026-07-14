; SYSCALL entry with interrupts masked by FMASK. Scheduling is single-core, so
; stack pointers use globals until SMP scheduling supplies per-CPU GS state.
; ponytail: replace these globals with KERNEL_GS_BASE/swapgs for SMP scheduling.
global syscall_entry
global g_syscall_kernel_rsp
extern syscall_dispatch

section .bss
g_syscall_kernel_rsp: resq 1
g_syscall_user_rsp:   resq 1

section .text
syscall_entry:
    mov [rel g_syscall_user_rsp], rsp
    mov rsp, [rel g_syscall_kernel_rsp]
    push qword [rel g_syscall_user_rsp]
    push rcx
    push r11
    ; syscall_dispatch is a SysV C function and clobbers the caller-saved argument
    ; registers; restore them so user mode never sees leftover kernel values.
    push rdi
    push rsi
    push rdx
    push r8
    push r9
    push r10
    mov rsi, rdi
    mov rdi, rax
    sub rsp, 8
    call syscall_dispatch
    add rsp, 8
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rsi
    pop rdi
    pop r11
    pop rcx
    pop rsp
    o64 sysret
