bits 64
section .rodata
global user_payload_start
global user_payload_end
user_payload_start:
    mov eax, 1        ; SYS_YIELD
    syscall
    mov eax, 1        ; SYS_YIELD
    syscall
    lea rbx, [rel .msg]  ; rbx: callee-saved, survives the syscall's C dispatch
.putc:
    movzx edi, byte [rbx]
    test edi, edi
    jz .sleep
    mov eax, 3        ; SYS_DEBUG_PUTC
    syscall
    inc rbx
    jmp .putc
.sleep:
    mov edi, 20
    mov eax, 2        ; SYS_SLEEP
    syscall
    push rax          ; touch the demand-paged user stack
    pop rax
    xor eax, eax      ; SYS_EXIT
    syscall
.msg: db "hello from userspace", 10, 0
user_payload_end:
