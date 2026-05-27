#include "kernel/crash.h"

#include <stddef.h>
#include <stdint.h>

#include <ktl/fmt>

#include "kernel/drivers/uart.h"
#include "kernel/log.h"
#include "kernel/x86/ioport.h"

extern kernel::driver::uart uart;

namespace kernel::crash::arch {
struct fp_walk_result {
    static constexpr size_t max_frames = 32;
    size_t depth                       = 0;
    uintptr_t frames[max_frames]       = {};
};
fp_walk_result walk_frame_pointers(uintptr_t start_rbp);
void read_control_registers(uint64_t out[4]);
const char* exception_name(uint32_t vec);
const char* trigger_name(kernel::crash::trigger_kind kind);
}  // namespace kernel::crash::arch

namespace kernel::crash {

namespace {

constexpr size_t kDumpFmtBufSize = 1536;
constexpr size_t kStackBytes     = 256;

bool g_in_dump                   = false;
bool g_harness_enabled           = false;
const char* g_test_name          = nullptr;

char g_fmt_buf[kDumpFmtBufSize];

template <typename... Args> void crash_emit(const char* fmt, const Args&... args) {
    ktl::format::format_to_buffer_raw(g_fmt_buf, sizeof(g_fmt_buf), fmt, args...);
    crash_write(g_fmt_buf);
}

const char* level_letter(kernel::log_level lvl) {
    switch (lvl) {
        case kernel::log_level::trace: return "t";
        case kernel::log_level::debug: return "d";
        case kernel::log_level::info: return "i";
        case kernel::log_level::warn: return "w";
        case kernel::log_level::error: return "E";
        case kernel::log_level::fatal: return "F";
        default: return "?";
    }
}

void emit_header_harness(trigger_kind kind, register_frame_t* regs, const char* message, const char* file, int line) {
    uint64_t vec  = regs ? regs->int_no : 0;
    uint64_t err  = regs ? regs->err_code : 0;
    uint64_t time = static_cast<uint64_t>(kernel::time::ns_since_boot());

    crash_write("\n@@CRASH_BEGIN {\"trigger\":\"");
    crash_write(arch::trigger_name(kind));
    crash_write("\"");
    if (regs) { crash_emit(",\"vec\":{0},\"err\":{1}", vec, err); }
    if (g_test_name) { crash_emit(",\"test\":\"{0}\"", g_test_name); }
    if (message) { crash_emit(",\"message\":\"{0}\"", message); }
    if (file) { crash_emit(",\"file\":\"{0}\",\"line\":{1}", file, line); }
    crash_emit(",\"ts\":{0}}}\n", time);
}

void emit_header_prose(trigger_kind kind, register_frame_t* regs, const char* message, const char* file, int line) {
    crash_write("\n*** KERNEL CRASH ***\n");
    crash_emit("Trigger: {0}", arch::trigger_name(kind));
    if (regs) {
        crash_emit(" (vec={0} {1}, err=0x{2:016p})\n", regs->int_no,
                   arch::exception_name(static_cast<uint32_t>(regs->int_no)), regs->err_code);
    } else {
        crash_write("\n");
    }
    if (message) { crash_emit("Message: {0}\n", message); }
    if (file) { crash_emit("At:      {0}:{1}\n", file, line); }
    if (g_test_name) {
        crash_emit("Test:    {0}\n", g_test_name);
    } else {
        crash_write("Test:    (none)\n");
    }
}

void emit_registers_harness(register_frame_t* regs, const uint64_t cr[4]) {
    if (!regs) return;
    crash_emit(
        "@@CRASH_REG {{\"rip\":\"0x{0:016p}\",\"rsp\":\"0x{1:016p}\",\"rbp\":\"0x{2:016p}\","
        "\"rflags\":\"0x{3:016p}\",\"cs\":\"0x{4:016p}\",\"ss\":\"0x{5:016p}\",",
        regs->rip, regs->userrsp, regs->rbp, regs->eflags, regs->cs, regs->ss);
    crash_emit(
        "\"rax\":\"0x{0:016p}\",\"rbx\":\"0x{1:016p}\",\"rcx\":\"0x{2:016p}\","
        "\"rdx\":\"0x{3:016p}\",\"rsi\":\"0x{4:016p}\",\"rdi\":\"0x{5:016p}\",",
        regs->rax, regs->rbx, regs->rcx, regs->rdx, regs->rsi, regs->rdi);
    crash_emit(
        "\"r8\":\"0x{0:016p}\",\"r9\":\"0x{1:016p}\",\"r10\":\"0x{2:016p}\","
        "\"r11\":\"0x{3:016p}\",\"r12\":\"0x{4:016p}\",\"r13\":\"0x{5:016p}\","
        "\"r14\":\"0x{6:016p}\",\"r15\":\"0x{7:016p}\",",
        regs->r8, regs->r9, regs->r10, regs->r11, regs->r12, regs->r13, regs->r14, regs->r15);
    crash_emit("\"cr0\":\"0x{0:016p}\",\"cr2\":\"0x{1:016p}\",\"cr3\":\"0x{2:016p}\",\"cr4\":\"0x{3:016p}\"}}\n", cr[0],
               cr[1], cr[2], cr[3]);
}

void emit_registers_prose(register_frame_t* regs, const uint64_t cr[4]) {
    if (!regs) {
        crash_write("Registers: (no register frame, voluntary panic)\n");
        return;
    }
    crash_write("Registers:\n");
    crash_emit("* rip=0x{0:016p}  rfl=0x{1:016p}\n", regs->rip, regs->eflags);
    crash_emit("* rsp=0x{0:016p}  rbp=0x{1:016p}\n", regs->userrsp, regs->rbp);
    crash_emit("* rax=0x{0:016p}  rbx=0x{1:016p}  rcx=0x{2:016p}\n", regs->rax, regs->rbx, regs->rcx);
    crash_emit("* rdx=0x{0:016p}  rsi=0x{1:016p}  rdi=0x{2:016p}\n", regs->rdx, regs->rsi, regs->rdi);
    crash_emit("*  r8=0x{0:016p}   r9=0x{1:016p}  r10=0x{2:016p}\n", regs->r8, regs->r9, regs->r10);
    crash_emit("* r11=0x{0:016p}  r12=0x{1:016p}  r13=0x{2:016p}\n", regs->r11, regs->r12, regs->r13);
    crash_emit("* r14=0x{0:016p}  r15=0x{1:016p}\n", regs->r14, regs->r15);
    crash_emit("*  cs=0x{0:016p}   ss=0x{1:016p}\n", regs->cs, regs->ss);
    crash_emit("* cr0=0x{0:016p}  cr2=0x{1:016p}\n", cr[0], cr[1]);
    crash_emit("* cr3=0x{0:016p}  cr4=0x{1:016p}\n", cr[2], cr[3]);
}

void emit_backtrace(const arch::fp_walk_result& bt, bool harness) {
    if (harness) {
        for (size_t i = 0; i < bt.depth; i++) {
            crash_emit("@@CRASH_FRAME {{\"i\":{0},\"addr\":\"0x{1:016p}\"}}\n", i, bt.frames[i]);
        }
    } else {
        crash_emit("Backtrace ({0} frames, frame-pointer walk):\n", bt.depth);
        for (size_t i = 0; i < bt.depth; i++) { crash_emit("  [{0}] 0x{1:016p}\n", i, bt.frames[i]); }
        if (bt.depth == 0) { crash_write("  (no frames -- rbp invalid or no frame pointer chain)\n"); }
    }
}

void emit_stack(register_frame_t* regs, bool harness) {
    if (!regs) return;
    uintptr_t rsp = regs->userrsp;

    // Range-check rsp before dereferencing. Kernel stacks live in higher half.
    if (rsp < 0xffff800000000000ULL || (rsp & 0x7) != 0) {
        if (harness) {
            crash_emit("@@CRASH_STACK {{\"rsp\":\"0x{0:016p}\",\"len\":0,\"hex\":\"\"}}\n", rsp);
        } else {
            crash_emit("Stack: (rsp=0x{0:016p} invalid, skipping)\n", rsp);
        }
        return;
    }

    if (harness) {
        crash_emit("@@CRASH_STACK {{\"rsp\":\"0x{0:016p}\",\"len\":{1},\"hex\":\"", rsp, kStackBytes);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(rsp);
        for (size_t i = 0; i < kStackBytes; i++) { crash_emit("{0:02x}", static_cast<unsigned>(p[i])); }
        crash_write("\"}\n");
    } else {
        crash_emit("Stack ({0} bytes from rsp=0x{1:016p}):\n", kStackBytes, rsp);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(rsp);
        for (size_t row = 0; row < kStackBytes; row += 16) {
            crash_emit("  +0x{0:03x}: ", row);
            for (size_t col = 0; col < 16 && row + col < kStackBytes; col++) {
                crash_emit("{0:02x} ", static_cast<unsigned>(p[row + col]));
            }
            crash_write("\n");
        }
    }
}

void emit_log_drain(bool harness) {
    // Walk g_log.messages directly. No locks (single-CPU at crash time, recursion guard
    // prevents reentrance). Iterating after acquiring the semaphore could deadlock if the
    // panic originated from inside the log path.
    g_log.messages.for_each([&](const kernel::log_message& msg) {
        time_ns_t ns = kernel::time::ktime_to_ns(msg.timestamp);
        if (harness) {
            crash_emit("@@CRASH_LOG {{\"seq\":{0},\"ns\":{1},\"lvl\":\"{2}\",\"text\":\"{3}\"}}\n", msg.sequence(), ns,
                       level_letter(msg.level()), msg.text.c_str());
        } else {
            uint64_t sec = static_cast<uint64_t>(ns) / 1'000'000'000ULL;
            uint64_t ms  = (static_cast<uint64_t>(ns) / 1'000'000ULL) % 1'000ULL;
            crash_emit("  [{0:03d}.{1:03d} {2}] {3}\n", sec, ms, level_letter(msg.level()), msg.text.c_str());
        }
    });
}

[[noreturn]] void terminate(trigger_kind kind) {
    unsigned char code = static_cast<unsigned char>(kind);
    if (g_harness_enabled) { outw(0x604, static_cast<uint16_t>(code) | 0x2000); }
    asm volatile("cli");
    for (;;) { asm volatile("hlt"); }
}

}  // namespace

void set_harness_enabled(bool enabled) { g_harness_enabled = enabled; }
void set_test_name(const char* name) { g_test_name = name; }

void crash_write(const char* s) {
    while (*s) { uart.write_byte(*s++); }
}

void crash_write_n(const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) { uart.write_byte(s[i]); }
}

[[noreturn]] void dispatch(trigger_kind kind, register_frame_t* regs, const char* message, const char* file, int line) {
    if (g_in_dump) {
        // Recursive crash inside the dumper -- abandon and halt.
        crash_write("\n*** RECURSIVE CRASH -- HALTING ***\n");
        asm volatile("cli");
        for (;;) { asm volatile("hlt"); }
    }
    g_in_dump    = true;

    bool harness = g_harness_enabled;
    uint64_t cr[4];
    arch::read_control_registers(cr);

    arch::fp_walk_result bt;
    if (regs) { bt = arch::walk_frame_pointers(regs->rbp); }

    if (harness) {
        emit_header_harness(kind, regs, message, file, line);
        emit_registers_harness(regs, cr);
        emit_backtrace(bt, true);
        emit_stack(regs, true);
        emit_log_drain(true);
        crash_write("@@CRASH_END {}\n");

        if (g_test_name) {
            crash_emit("@@HARNESS {{\"event\":\"test_crash\",\"name\":\"{0}\",\"trigger\":\"{1}\"}}\n", g_test_name,
                       arch::trigger_name(kind));
        }
        crash_emit("@@HARNESS {{\"event\":\"abort\",\"code\":{0}}}\n", static_cast<unsigned>(kind));
    } else {
        emit_header_prose(kind, regs, message, file, line);
        emit_registers_prose(regs, cr);
        emit_backtrace(bt, false);
        emit_stack(regs, false);
        crash_write("Recent log:\n");
        emit_log_drain(false);
        crash_write("*** halted ***\n");
    }

    terminate(kind);
}

}  // namespace kernel::crash
