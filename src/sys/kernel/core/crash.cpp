#include "kernel/crash.h"

#include <stddef.h>
#include <stdint.h>

#include <ktl/fmt>
#include <ktl/ranges>
#include <ktl/span>

#include "kernel/arch.h"
#include "kernel/drivers/uart.h"
#include "kernel/json_escape.h"
#include "kernel/log.h"
#include "kernel/panic.h"
#include "kernel/symbols.h"

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

const char* display_name(const char* mangled, char* scratch, size_t scratch_size) {
    if (mangled == nullptr) { return nullptr; }
    if (kernel::symbols::demangle(mangled, ktl::span(scratch, scratch_size))) { return scratch; }
    return mangled;
}

void emit_backtrace(const arch::fp_walk_result& bt, bool harness) {
    char name_buf[256];
    if (harness) {
        for (auto [i, frame] : ktl::views::enumerate(ktl::span(bt.frames, bt.depth))) {
            auto sym = kernel::symbols::lookup(frame);
            if (sym) {
                const char* pretty = display_name(sym->name, name_buf, sizeof(name_buf));
                crash_emit("@@CRASH_FRAME {{\"i\":{0},\"addr\":\"0x{1:016p}\",\"sym\":\"{2}\",\"off\":{3}}}\n", i,
                           frame, pretty, sym->offset);
            } else {
                crash_emit("@@CRASH_FRAME {{\"i\":{0},\"addr\":\"0x{1:016p}\"}}\n", i, frame);
            }
        }
    } else {
        crash_emit("Backtrace ({0} frames, frame-pointer walk):\n", bt.depth);
        for (auto [i, frame] : ktl::views::enumerate(ktl::span(bt.frames, bt.depth))) {
            auto sym = kernel::symbols::lookup(frame);
            if (sym) {
                const char* pretty = display_name(sym->name, name_buf, sizeof(name_buf));
                crash_emit("  [{0}] 0x{1:016p}  {2} +0x{3:x}\n", i, frame, pretty, sym->offset);
            } else {
                crash_emit("  [{0}] 0x{1:016p}\n", i, frame);
            }
        }
        if (bt.depth == 0) { crash_write("  (no frames -- rbp invalid or no frame pointer chain)\n"); }
    }
}

void emit_stack(register_frame_t* regs, bool harness) {
    if (!regs) return;
    uintptr_t rsp = regs->userrsp;

    // Range-check rsp before dereferencing. Kernel stacks live in higher half;
    // also reject rsp so close to the top of the address space that the window wraps.
    if (rsp < 0xffff800000000000ULL || (rsp & 0x7) != 0 || rsp > UINTPTR_MAX - kStackBytes) {
        if (harness) {
            crash_emit("@@CRASH_STACK {{\"rsp\":\"0x{0:016p}\",\"len\":0,\"hex\":\"\"}}\n", rsp);
        } else {
            crash_emit("Stack: (rsp=0x{0:016p} invalid, skipping)\n", rsp);
        }
        return;
    }

    // Probe each page in [rsp, rsp + kStackBytes) and dump only the mapped prefix,
    // so a stack window ending near an unmapped page does not page-fault inside the dump.
    size_t len = 0;
    while (len < kStackBytes) {
        uintptr_t addr = rsp + len;
        if (!arch::probe_readable(addr)) break;
        size_t in_page = static_cast<size_t>(0x1000 - (addr & 0xFFF));
        size_t left    = kStackBytes - len;
        len += in_page < left ? in_page : left;
    }

    if (harness) {
        crash_emit("@@CRASH_STACK {{\"rsp\":\"0x{0:016p}\",\"len\":{1},\"hex\":\"", rsp, len);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(rsp);
        for (size_t i = 0; i < len; i++) { crash_emit("{0:02x}", static_cast<unsigned>(p[i])); }
        crash_write("\"}\n");
    } else {
        crash_emit("Stack ({0} bytes from rsp=0x{1:016p}):\n", len, rsp);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(rsp);
        for (size_t row = 0; row < len; row += 16) {
            crash_emit("  +0x{0:03x}: ", row);
            for (size_t col = 0; col < 16 && row + col < len; col++) {
                crash_emit("{0:02x} ", static_cast<unsigned>(p[row + col]));
            }
            crash_write("\n");
        }
        if (len < kStackBytes) { crash_emit("  (truncated at +0x{0:03x}: page unmapped)\n", len); }
    }
}

void emit_log_drain(bool harness) {
    // The producer path holds no lock; force the flush flag (its holder may be the flush that
    // just faulted) and scan the retained window. In-progress slots are shown as a placeholder
    // since their bytes may be torn. SMP: a peer core mid-log during the dump races this scan;
    // that quiescing is deferred (best-effort dump) -- see the spec. Single-core (the only
    // configuration today) has no such peer.
    g_log.crash_for_each([&](const kernel::log_message* msg, bool in_progress) {
        if (in_progress) {
            if (harness) {
                crash_write("@@CRASH_LOG {\"seq\":0,\"ns\":0,\"lvl\":\"?\",\"text\":\"<in-progress>\"}\n");
            } else {
                crash_write("  [in-progress writer]\n");
            }
            return;
        }
        time_ns_t ns = kernel::time::ktime_to_ns(msg->timestamp);
        if (harness) {
            // Emit in pieces so the user-controlled text is JSON-escaped (a quote/newline in a log
            // line -- e.g. an assertion's stringized expression -- would otherwise break the record).
            crash_emit("@@CRASH_LOG {{\"seq\":{0},\"ns\":{1},\"lvl\":\"{2}\",\"text\":\"", msg->sequence(), ns,
                       level_letter(msg->level()));
            kernel::write_json_escaped([](char ch) { uart.write_byte(ch); }, msg->text.c_str());
            crash_write("\"}\n");
        } else {
            uint64_t sec = static_cast<uint64_t>(ns) / 1'000'000'000ULL;
            uint64_t ms  = (static_cast<uint64_t>(ns) / 1'000'000ULL) % 1'000ULL;
            crash_emit("  [{0:03d}.{1:03d} {2}] {3}\n", sec, ms, level_letter(msg->level()), msg->text.c_str());
        }
    });
}

[[noreturn]] void terminate(trigger_kind kind) {
    if (g_harness_enabled) { kernel::arch::harness_exit(static_cast<uint8_t>(kind)); }
    hcf();
}

}  // namespace

void set_harness_enabled(bool enabled) { g_harness_enabled = enabled; }
void set_test_name(const char* name) { g_test_name = name; }

void crash_write(const char* s) {
    while (*s) { uart.write_byte(*s++); }
}

void crash_write_n(const char* s, size_t n) {
    for (char c : ktl::span(s, n)) { uart.write_byte(c); }
}

[[noreturn]] void dispatch(trigger_kind kind, register_frame_t* regs, const char* message, const char* file, int line) {
    if (g_in_dump) {
        // Recursive crash inside the dumper -- abandon and halt.
        crash_write("\n*** RECURSIVE CRASH -- HALTING ***\n");
        if (g_harness_enabled) {
            // Emit a minimal terminator so harness mode sees a structured abort
            // instead of timing out on the spin loop below.
            crash_write("@@CRASH_END {}\n");
            crash_emit("@@HARNESS {{\"event\":\"abort\",\"code\":{0}}}\n", static_cast<unsigned>(kind));
            terminate(kind);
        }
        hcf();
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
