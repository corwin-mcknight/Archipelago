#include "kernel/crash.h"

#include <stddef.h>
#include <stdint.h>

#include <ktl/fmt>

// HHDM (higher-half direct map) offset published by the Limine boot path (riscv64/main.cpp).
extern uintptr_t g_hhdm_offset;

namespace kernel::crash::arch {

struct fp_walk_result {
    static constexpr size_t max_frames = 32;
    size_t depth                       = 0;
    uintptr_t frames[max_frames]       = {};
};

namespace {

// Sv48 page-table entry bits, duplicated locally so the crash path stays self-contained.
constexpr uint64_t PTE_VALID = 1ull << 0;
constexpr uint64_t PTE_READ  = 1ull << 1;
constexpr uint64_t PTE_WRITE = 1ull << 2;
constexpr uint64_t PTE_EXEC  = 1ull << 3;
constexpr uint64_t PTE_RWX   = PTE_READ | PTE_WRITE | PTE_EXEC;

uint64_t pte_paddr(uint64_t entry) { return (entry >> 10) << 12; }

bool is_canonical(uintptr_t vaddr) {
    int64_t s = static_cast<int64_t>(vaddr) >> 47;
    return s == 0 || s == -1;
}

}  // namespace

bool probe_readable(uintptr_t vaddr) {
    if (!is_canonical(vaddr)) { return false; }
    // Before the HHDM offset is known we cannot reach the page tables; report unmapped
    // so the dumper degrades (skips the read) instead of faulting.
    if (g_hhdm_offset == 0) { return false; }

    uint64_t satp = 0;
    asm volatile("csrr %0, satp" : "=r"(satp));
    uint64_t table_phys = (satp & ((1ull << 44) - 1)) << 12;

    // Bounded 4-level Sv48 walk, shifts 39 -> 30 -> 21 -> 12. An entry with any
    // of R/W/X set is a leaf at that level; a valid pointer entry has none.
    for (int shift = 39; shift >= 12; shift -= 9) {
        const uint64_t* table = reinterpret_cast<const uint64_t*>(table_phys + g_hhdm_offset);
        uint64_t entry        = table[(vaddr >> shift) & 0x1FF];
        if (!(entry & PTE_VALID)) { return false; }
        if (entry & PTE_RWX) { return (entry & PTE_READ) != 0; }
        table_phys = pte_paddr(entry);
    }
    return false;  // deepest level held a pointer entry -- malformed table
}

static bool in_kernel_half(uintptr_t p) {
    // Coarse: any canonical higher-half address. Rejects null/userspace/garbage.
    return p >= 0xffff800000000000ULL;
}

static bool plausible_fp(uintptr_t p) {
    // fp points at the frame record's one-past-the-end (the CFA) -- must be 8-aligned.
    return in_kernel_half(p) && (p & 0x7) == 0;
}

fp_walk_result walk_frame_pointers(uintptr_t start_fp) {
    fp_walk_result out{};
    uintptr_t fp      = start_fp;
    uintptr_t prev_fp = 0;

    for (size_t i = 0; i < fp_walk_result::max_frames; i++) {
        if (!plausible_fp(fp)) break;
        if (fp == prev_fp) break;                  // self-loop
        if (prev_fp != 0 && fp <= prev_fp) break;  // chain must climb the stack

        // riscv frame record sits below fp: [fp-8] = return address, [fp-16] = caller fp.
        // Both slots must sit on mapped pages -- a corrupt fp must not page-fault
        // inside the dump. Each 8-aligned uint64 lies within a single page.
        if (!probe_readable(fp - 8) || !probe_readable(fp - 16)) break;

        uintptr_t ret     = *reinterpret_cast<uintptr_t*>(fp - 8);
        uintptr_t next_fp = *reinterpret_cast<uintptr_t*>(fp - 16);
        if (!in_kernel_half(ret)) break;  // return addresses are byte-aligned

        out.frames[out.depth++] = ret;
        prev_fp                 = fp;
        fp                      = next_fp;
    }
    return out;
}

void read_control_registers(uint64_t out[4]) {
    asm volatile("csrr %0, sstatus" : "=r"(out[0]));
    asm volatile("csrr %0, stval" : "=r"(out[1]));
    asm volatile("csrr %0, satp" : "=r"(out[2]));
    asm volatile("csrr %0, scause" : "=r"(out[3]));
}

const char* exception_name(uint32_t vec) {
    switch (vec) {
        case 0: return "instruction address misaligned";
        case 1: return "instruction access fault";
        case 2: return "illegal instruction";
        case 3: return "breakpoint";
        case 4: return "load address misaligned";
        case 5: return "load access fault";
        case 6: return "store address misaligned";
        case 7: return "store access fault";
        case 8: return "ecall from U-mode";
        case 9: return "ecall from S-mode";
        case 12: return "instruction page fault";
        case 13: return "load page fault";
        case 15: return "store page fault";
        default: return "unknown";
    }
}

uint64_t frame_vec(register_frame_t* regs) { return regs->scause; }
uint64_t frame_err(register_frame_t* regs) { return regs->stval; }
uintptr_t frame_fp(register_frame_t* regs) { return regs->s0; }
uintptr_t frame_sp(register_frame_t* regs) { return regs->sp; }

namespace {
char g_reg_fmt_buf[512];
template <typename... Args> void emit(const char* fmt, const Args&... args) {
    ktl::format::format_to_buffer_raw(g_reg_fmt_buf, sizeof(g_reg_fmt_buf), fmt, args...);
    kernel::crash::crash_write(g_reg_fmt_buf);
}
}  // namespace

// cr[] carries {sstatus, stval, satp, scause} (see read_control_registers).
void emit_registers_harness(register_frame_t* regs, const uint64_t cr[4]) {
    emit(
        "@@CRASH_REG {{\"pc\":\"0x{0:016p}\",\"sp\":\"0x{1:016p}\",\"fp\":\"0x{2:016p}\","
        "\"ra\":\"0x{3:016p}\",\"gp\":\"0x{4:016p}\",\"tp\":\"0x{5:016p}\",",
        regs->sepc, regs->sp, regs->s0, regs->ra, regs->gp, regs->tp);
    emit(
        "\"a0\":\"0x{0:016p}\",\"a1\":\"0x{1:016p}\",\"a2\":\"0x{2:016p}\",\"a3\":\"0x{3:016p}\","
        "\"a4\":\"0x{4:016p}\",\"a5\":\"0x{5:016p}\",\"a6\":\"0x{6:016p}\",\"a7\":\"0x{7:016p}\",",
        regs->a0, regs->a1, regs->a2, regs->a3, regs->a4, regs->a5, regs->a6, regs->a7);
    emit(
        "\"t0\":\"0x{0:016p}\",\"t1\":\"0x{1:016p}\",\"t2\":\"0x{2:016p}\",\"t3\":\"0x{3:016p}\","
        "\"t4\":\"0x{4:016p}\",\"t5\":\"0x{5:016p}\",\"t6\":\"0x{6:016p}\",",
        regs->t0, regs->t1, regs->t2, regs->t3, regs->t4, regs->t5, regs->t6);
    emit(
        "\"s1\":\"0x{0:016p}\",\"s2\":\"0x{1:016p}\",\"s3\":\"0x{2:016p}\",\"s4\":\"0x{3:016p}\","
        "\"s5\":\"0x{4:016p}\",\"s6\":\"0x{5:016p}\",\"s7\":\"0x{6:016p}\",\"s8\":\"0x{7:016p}\",",
        regs->s1, regs->s2, regs->s3, regs->s4, regs->s5, regs->s6, regs->s7, regs->s8);
    emit("\"s9\":\"0x{0:016p}\",\"s10\":\"0x{1:016p}\",\"s11\":\"0x{2:016p}\",", regs->s9, regs->s10, regs->s11);
    emit("\"sstatus\":\"0x{0:016p}\",\"stval\":\"0x{1:016p}\",\"satp\":\"0x{2:016p}\",\"scause\":\"0x{3:016p}\"}}\n",
         cr[0], cr[1], cr[2], cr[3]);
}

void emit_registers_prose(register_frame_t* regs, const uint64_t cr[4]) {
    kernel::crash::crash_write("Registers:\n");
    emit("*  pc=0x{0:016p}   ra=0x{1:016p}\n", regs->sepc, regs->ra);
    emit("*  sp=0x{0:016p}   fp=0x{1:016p}\n", regs->sp, regs->s0);
    emit("*  gp=0x{0:016p}   tp=0x{1:016p}\n", regs->gp, regs->tp);
    emit("*  a0=0x{0:016p}   a1=0x{1:016p}   a2=0x{2:016p}\n", regs->a0, regs->a1, regs->a2);
    emit("*  a3=0x{0:016p}   a4=0x{1:016p}   a5=0x{2:016p}\n", regs->a3, regs->a4, regs->a5);
    emit("*  a6=0x{0:016p}   a7=0x{1:016p}\n", regs->a6, regs->a7);
    emit("*  t0=0x{0:016p}   t1=0x{1:016p}   t2=0x{2:016p}\n", regs->t0, regs->t1, regs->t2);
    emit("*  t3=0x{0:016p}   t4=0x{1:016p}   t5=0x{2:016p}\n", regs->t3, regs->t4, regs->t5);
    emit("*  t6=0x{0:016p}   s1=0x{1:016p}   s2=0x{2:016p}\n", regs->t6, regs->s1, regs->s2);
    emit("*  s3=0x{0:016p}   s4=0x{1:016p}   s5=0x{2:016p}\n", regs->s3, regs->s4, regs->s5);
    emit("*  s6=0x{0:016p}   s7=0x{1:016p}   s8=0x{2:016p}\n", regs->s6, regs->s7, regs->s8);
    emit("*  s9=0x{0:016p}  s10=0x{1:016p}  s11=0x{2:016p}\n", regs->s9, regs->s10, regs->s11);
    emit("* sstatus=0x{0:016p}  stval=0x{1:016p}\n", cr[0], cr[1]);
    emit("* satp=0x{0:016p}     scause=0x{1:016p}\n", cr[2], cr[3]);
}

}  // namespace kernel::crash::arch
