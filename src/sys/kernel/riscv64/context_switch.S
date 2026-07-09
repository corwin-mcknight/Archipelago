// src/sys/kernel/riscv64/context_switch.S
// Callee-saved context switch and the first-run entry trampoline. Frame layout (14 slots, 112
// bytes, 16-aligned) must match prepare_thread_stack() in riscv64/arch.cpp exactly:
// ra 0, s0 8, s1 16, s2 24, s3 32, s4 40, s5 48, s6 56, s7 64, s8 72, s9 80, s10 88, s11 96.

.section .text
.global arch_context_switch
.align 2
arch_context_switch:
    addi sp, sp, -112
    sd ra, 0(sp)
    sd s0, 8(sp)
    sd s1, 16(sp)
    sd s2, 24(sp)
    sd s3, 32(sp)
    sd s4, 40(sp)
    sd s5, 48(sp)
    sd s6, 56(sp)
    sd s7, 64(sp)
    sd s8, 72(sp)
    sd s9, 80(sp)
    sd s10, 88(sp)
    sd s11, 96(sp)
    sd sp, 0(a0)                # publish outgoing sp
    mv sp, a1                   # adopt incoming stack
    ld ra, 0(sp)
    ld s0, 8(sp)
    ld s1, 16(sp)
    ld s2, 24(sp)
    ld s3, 32(sp)
    ld s4, 40(sp)
    ld s5, 48(sp)
    ld s6, 56(sp)
    ld s7, 64(sp)
    ld s8, 72(sp)
    ld s9, 80(sp)
    ld s10, 88(sp)
    ld s11, 96(sp)
    addi sp, sp, 112
    ret

// First run of a spawned thread: s1=entry, s2=arg were seeded by prepare_thread_stack.
.global thread_entry_trampoline
.align 2
thread_entry_trampoline:
    call sched_finish_switch
    csrsi sstatus, 2            # SIE: interrupts on
    mv a0, s2
    jalr s1
    call sched_thread_exit
