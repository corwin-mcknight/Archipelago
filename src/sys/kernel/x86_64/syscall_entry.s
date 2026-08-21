global syscall_entry
extern syscall_dispatch

section .text
syscall_entry:
    ; GS points at cpu_local: kernel_rsp is +8 and user_rsp scratch is +16.
    mov [gs:16], rsp
    mov rsp, [gs:8]
    push qword [gs:16]
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
    ; User ABI: rax = number, rdi/rsi/rdx/r10/r8/r9 = args 0..5. r10 stands in for rcx on the
    ; user side because SYSCALL itself parks the return address in rcx; the pushed copy above
    ; restores it.
    ;
    ; syscall_dispatch takes seven parameters, one more than SysV passes in registers, so a5
    ; travels on the stack. Pushing it also supplies the 8 bytes of realignment the call needs
    ; (nine pushes above leave rsp misaligned), which is why there is no separate sub rsp, 8.
    ;
    ; The moves run in an order where every register is read before the move that overwrites it.
    push r9        ; a5 -> stack slot, and the call's alignment
    mov r9, r8     ; a4
    mov r8, r10    ; a3
    mov rcx, rdx   ; a2
    mov rdx, rsi   ; a1
    mov rsi, rdi   ; a0
    mov rdi, rax   ; number
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
