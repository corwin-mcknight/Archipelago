// Supervisor trap entry: build a register_frame_t on the kernel stack and hand
// it to riscv_trap_handler. Byte offsets must match the field order in
// includes/kernel/riscv/registers.h exactly. Kernel-mode only for now -- traps
// are taken on the current stack (no sscratch swap until user mode exists).

.section .text
.global riscv_trap_entry
.align 2
riscv_trap_entry:
    // Reentrancy backstop: a stack overflow re-faults on the first frame
    // store below, which would recurse here forever. If sp has fallen below
    // the boot stack's low-water mark, divert to the emergency stack and
    // panic instead of corrupting whatever mapping sp lands on. Floor is 0
    // until trap_init runs, which keeps the check inert.
    csrw sscratch, t0
    la   t0, g_kstack_floor
    ld   t0, 0(t0)
    bltu sp, t0, trap_sp_overflow
    csrr t0, sscratch

    addi sp, sp, -288

    sd ra, 0(sp)
    sd gp, 16(sp)
    sd tp, 24(sp)
    sd t0, 32(sp)
    sd t1, 40(sp)
    sd t2, 48(sp)
    sd s0, 56(sp)
    sd s1, 64(sp)
    sd a0, 72(sp)
    sd a1, 80(sp)
    sd a2, 88(sp)
    sd a3, 96(sp)
    sd a4, 104(sp)
    sd a5, 112(sp)
    sd a6, 120(sp)
    sd a7, 128(sp)
    sd s2, 136(sp)
    sd s3, 144(sp)
    sd s4, 152(sp)
    sd s5, 160(sp)
    sd s6, 168(sp)
    sd s7, 176(sp)
    sd s8, 184(sp)
    sd s9, 192(sp)
    sd s10, 200(sp)
    sd s11, 208(sp)
    sd t3, 216(sp)
    sd t4, 224(sp)
    sd t5, 232(sp)
    sd t6, 240(sp)

    addi t0, sp, 288        // pre-trap stack pointer
    sd t0, 8(sp)

    csrr t0, sepc
    sd t0, 248(sp)
    csrr t0, sstatus
    sd t0, 256(sp)
    csrr t0, scause
    sd t0, 264(sp)
    csrr t0, stval
    sd t0, 272(sp)

    mv a0, sp
    call riscv_trap_handler

    // The handler may have advanced sepc (e.g. to skip a breakpoint) or be
    // returning into a demand-paged retry; write the CSRs back before sret.
    ld t0, 248(sp)
    csrw sepc, t0
    ld t0, 256(sp)
    csrw sstatus, t0

    ld ra, 0(sp)
    ld gp, 16(sp)
    ld tp, 24(sp)
    ld t1, 40(sp)
    ld t2, 48(sp)
    ld s0, 56(sp)
    ld s1, 64(sp)
    ld a0, 72(sp)
    ld a1, 80(sp)
    ld a2, 88(sp)
    ld a3, 96(sp)
    ld a4, 104(sp)
    ld a5, 112(sp)
    ld a6, 120(sp)
    ld a7, 128(sp)
    ld s2, 136(sp)
    ld s3, 144(sp)
    ld s4, 152(sp)
    ld s5, 160(sp)
    ld s6, 168(sp)
    ld s7, 176(sp)
    ld s8, 184(sp)
    ld s9, 192(sp)
    ld s10, 200(sp)
    ld s11, 208(sp)
    ld t3, 216(sp)
    ld t4, 224(sp)
    ld t5, 232(sp)
    ld t6, 240(sp)
    ld t0, 32(sp)

    addi sp, sp, 288
    sret

// Registers are unrecoverable here (the faulting context already lost sp);
// grab the trap CSRs as arguments and hand off to a noreturn panic.
trap_sp_overflow:
    la   sp, emergency_stack_top
    csrr a0, sepc
    csrr a1, stval
    call riscv_trap_stack_overflow

.section .bss
.align 4
emergency_stack:
    .space 4096
emergency_stack_top:
