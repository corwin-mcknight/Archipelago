.section .rodata
.globl user_payload_start
.globl user_payload_end
user_payload_start:
    li a7, 1              // SYS_YIELD
    ecall
    li a7, 1              // SYS_YIELD
    ecall
    la a1, 2f             // PC-relative; a1 survives ecall via the full trap frame
1:
    lbu a0, 0(a1)
    beqz a0, 3f
    li a7, 3              // SYS_DEBUG_PUTC
    ecall
    addi a1, a1, 1
    j 1b
3:
    li a0, 20
    li a7, 2              // SYS_SLEEP
    ecall
    addi sp, sp, -16      // touch the demand-paged user stack
    sd a0, 0(sp)
    addi sp, sp, 16
    li a7, 0              // SYS_EXIT
    ecall
2:  .asciz "hello from userspace\n"
user_payload_end:
