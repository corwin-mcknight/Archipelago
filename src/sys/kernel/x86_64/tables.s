global kernel_x86_install_gdt

kernel_x86_install_gdt:
    lgdt    [rdi]
    ; Set segement registers
    push    rax
    mov     ax,     0x28  
    ltr     ax
    mov     ax,     0x10
    mov     ss,     ax
    mov     ds,     ax
    mov     es,     ax
    mov     fs,     ax
    mov     gs,     ax

    ; Swap code segment
    mov     rax,    after_gdt
    push    0x08                    ; New CS
    push    rax                     ; "Return location"
    retfq                           ; Swap!

after_gdt:
    pop rax
    ret
