; src/sys/kernel/x86_64/context_switch.s
; Callee-saved context switch and the first-run entry trampoline. Frame layout must match
; prepare_thread_stack() in x86_64/arch.cpp exactly.
bits 64
default rel
section .text

global arch_context_switch
arch_context_switch:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp              ; publish outgoing sp
    mov rsp, rsi                ; adopt incoming stack
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

extern sched_finish_switch
extern sched_thread_exit

; First run of a spawned thread: prepare_thread_stack seeded rbx=entry, r12=arg and pointed the
; switch frame's return address here. Interrupts stay off until the scheduler bookkeeping is done.
global thread_entry_trampoline
thread_entry_trampoline:
    call sched_finish_switch
    sti
    mov rdi, r12
    call rbx
    call sched_thread_exit
