#include "kernel/crash.h"

#include <stddef.h>
#include <stdint.h>

#include <ktl/fmt>

// HHDM (higher-half direct map) offset published by the Limine boot path (x86_64/main.cpp).
extern uintptr_t g_hhdm_offset;

namespace kernel::crash::arch {

struct fp_walk_result {
    static constexpr size_t max_frames = 32;
    size_t depth                       = 0;
    uintptr_t frames[max_frames]       = {};
};

namespace {

// Page-table entry bits, duplicated locally so the crash path stays self-contained.
constexpr uint64_t PTE_PRESENT   = 1ull << 0;
constexpr uint64_t PTE_HUGE      = 1ull << 7;
constexpr uint64_t PTE_ADDR_MASK = 0x000FFFFFFFFFF000ull;

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

    uint64_t cr3 = 0;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t table_phys = cr3 & PTE_ADDR_MASK;

    // Bounded 4-level walk: PML4 (shift 39) -> PDPT (30) -> PD (21) -> PT (12).
    // Table physical addresses are read through the HHDM, never recursively.
    for (int shift = 39; shift >= 12; shift -= 9) {
        const uint64_t* table = reinterpret_cast<const uint64_t*>(table_phys + g_hhdm_offset);
        uint64_t entry        = table[(vaddr >> shift) & 0x1FF];
        if (!(entry & PTE_PRESENT)) { return false; }
        if (shift == 12) { return true; }
        // 1 GiB (PDPT) and 2 MiB (PD) leaf mappings terminate the walk early.
        if ((entry & PTE_HUGE) && (shift == 30 || shift == 21)) { return true; }
        table_phys = entry & PTE_ADDR_MASK;
    }
    return true;
}

static bool in_kernel_half(uintptr_t p) {
    // Coarse: any canonical higher-half address. Rejects null/userspace/garbage.
    return p >= 0xffff800000000000ULL;
}

static bool plausible_rbp(uintptr_t p) {
    // rbp points at a stack slot holding a uint64 -- must be 8-aligned.
    return in_kernel_half(p) && (p & 0x7) == 0;
}

fp_walk_result walk_frame_pointers(uintptr_t start_rbp) {
    fp_walk_result out{};
    uintptr_t rbp      = start_rbp;
    uintptr_t prev_rbp = 0;

    for (size_t i = 0; i < fp_walk_result::max_frames; i++) {
        if (!plausible_rbp(rbp)) break;
        if (rbp > UINTPTR_MAX - 16) break;            // frame[0]/frame[1] reads would wrap
        if (rbp == prev_rbp) break;                   // self-loop
        if (prev_rbp != 0 && rbp <= prev_rbp) break;  // chain must climb the stack

        // Both slots must sit on mapped pages -- a corrupt rbp must not page-fault
        // inside the dump. Each 8-aligned uint64 lies within a single page.
        if (!probe_readable(rbp) || !probe_readable(rbp + 8)) break;

        // Layout: [rbp+0] = saved rbp, [rbp+8] = return address.
        uintptr_t* frame = reinterpret_cast<uintptr_t*>(rbp);
        uintptr_t ret    = frame[1];
        if (!in_kernel_half(ret)) break;  // return addresses are byte-aligned

        out.frames[out.depth++] = ret;
        prev_rbp                = rbp;
        rbp                     = frame[0];
    }
    return out;
}

void read_control_registers(uint64_t out[4]) {
    asm volatile("mov %%cr0, %0" : "=r"(out[0]));
    asm volatile("mov %%cr2, %0" : "=r"(out[1]));
    asm volatile("mov %%cr3, %0" : "=r"(out[2]));
    asm volatile("mov %%cr4, %0" : "=r"(out[3]));
}

const char* exception_name(uint32_t vec) {
    switch (vec) {
        case 0: return "#DE divide error";
        case 1: return "#DB debug";
        case 2: return "NMI";
        case 3: return "#BP breakpoint";
        case 4: return "#OF overflow";
        case 5: return "#BR bound range";
        case 6: return "#UD invalid opcode";
        case 7: return "#NM device not available";
        case 8: return "#DF double fault";
        case 10: return "#TS invalid TSS";
        case 11: return "#NP segment not present";
        case 12: return "#SS stack-segment fault";
        case 13: return "#GP general protection";
        case 14: return "#PF page fault";
        case 16: return "#MF x87 floating-point";
        case 17: return "#AC alignment check";
        case 18: return "#MC machine check";
        case 19: return "#XF SIMD floating-point";
        case 20: return "#VE virtualization";
        case 21: return "#CP control protection";
        default: return "unknown";
    }
}

uint64_t frame_vec(register_frame_t* regs) { return regs->int_no; }
uint64_t frame_err(register_frame_t* regs) { return regs->err_code; }
uintptr_t frame_fp(register_frame_t* regs) { return regs->rbp; }
uintptr_t frame_sp(register_frame_t* regs) { return regs->userrsp; }

namespace {
char g_reg_fmt_buf[512];
template <typename... Args> void emit(const char* fmt, const Args&... args) {
    ktl::format::format_to_buffer_raw(g_reg_fmt_buf, sizeof(g_reg_fmt_buf), fmt, args...);
    kernel::crash::crash_write(g_reg_fmt_buf);
}
}  // namespace

// cr[] carries {cr0, cr2, cr3, cr4} (see read_control_registers).
void emit_registers_harness(register_frame_t* regs, const uint64_t cr[4]) {
    emit(
        "@@CRASH_REG {{\"rip\":\"0x{0:016p}\",\"rsp\":\"0x{1:016p}\",\"rbp\":\"0x{2:016p}\","
        "\"rflags\":\"0x{3:016p}\",\"cs\":\"0x{4:016p}\",\"ss\":\"0x{5:016p}\",",
        regs->rip, regs->userrsp, regs->rbp, regs->eflags, regs->cs, regs->ss);
    emit(
        "\"rax\":\"0x{0:016p}\",\"rbx\":\"0x{1:016p}\",\"rcx\":\"0x{2:016p}\","
        "\"rdx\":\"0x{3:016p}\",\"rsi\":\"0x{4:016p}\",\"rdi\":\"0x{5:016p}\",",
        regs->rax, regs->rbx, regs->rcx, regs->rdx, regs->rsi, regs->rdi);
    emit(
        "\"r8\":\"0x{0:016p}\",\"r9\":\"0x{1:016p}\",\"r10\":\"0x{2:016p}\","
        "\"r11\":\"0x{3:016p}\",\"r12\":\"0x{4:016p}\",\"r13\":\"0x{5:016p}\","
        "\"r14\":\"0x{6:016p}\",\"r15\":\"0x{7:016p}\",",
        regs->r8, regs->r9, regs->r10, regs->r11, regs->r12, regs->r13, regs->r14, regs->r15);
    emit("\"cr0\":\"0x{0:016p}\",\"cr2\":\"0x{1:016p}\",\"cr3\":\"0x{2:016p}\",\"cr4\":\"0x{3:016p}\"}}\n", cr[0],
         cr[1], cr[2], cr[3]);
}

void emit_registers_prose(register_frame_t* regs, const uint64_t cr[4]) {
    kernel::crash::crash_write("Registers:\n");
    emit("* rip=0x{0:016p}  rfl=0x{1:016p}\n", regs->rip, regs->eflags);
    emit("* rsp=0x{0:016p}  rbp=0x{1:016p}\n", regs->userrsp, regs->rbp);
    emit("* rax=0x{0:016p}  rbx=0x{1:016p}  rcx=0x{2:016p}\n", regs->rax, regs->rbx, regs->rcx);
    emit("* rdx=0x{0:016p}  rsi=0x{1:016p}  rdi=0x{2:016p}\n", regs->rdx, regs->rsi, regs->rdi);
    emit("*  r8=0x{0:016p}   r9=0x{1:016p}  r10=0x{2:016p}\n", regs->r8, regs->r9, regs->r10);
    emit("* r11=0x{0:016p}  r12=0x{1:016p}  r13=0x{2:016p}\n", regs->r11, regs->r12, regs->r13);
    emit("* r14=0x{0:016p}  r15=0x{1:016p}\n", regs->r14, regs->r15);
    emit("*  cs=0x{0:016p}   ss=0x{1:016p}\n", regs->cs, regs->ss);
    emit("* cr0=0x{0:016p}  cr2=0x{1:016p}\n", cr[0], cr[1]);
    emit("* cr3=0x{0:016p}  cr4=0x{1:016p}\n", cr[2], cr[3]);
}

}  // namespace kernel::crash::arch
