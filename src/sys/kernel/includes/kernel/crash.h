#pragma once

#include <stddef.h>
#include <stdint.h>

#include <ktl/fmt>

#include "kernel/registers.h"

namespace kernel::crash {

namespace arch {
// True if vaddr is mapped (present at every level of a software walk of the
// current page-table root) and thus safe for the crash dumper to read. Safe
// on garbage input: canonical check first, bounded walk.
bool probe_readable(uintptr_t vaddr);

// Frame-record slot offsets relative to the frame pointer: where the return
// address and the caller's frame pointer live. riscv64: {-8, -16}; x86_64:
// {+8, 0}.
extern const intptr_t frame_ret_slot;
extern const intptr_t frame_next_slot;

void read_control_registers(uint64_t out[4]);
const char* exception_name(uint32_t vec);
// Trap frame accessors: the trap/error identifiers the header reports, the
// frame pointer the backtrace starts from, the stack pointer the stack dump
// reads. Register rendering is fully per-arch; regs is never null.
uint64_t frame_vec(register_frame_t* regs);
uint64_t frame_err(register_frame_t* regs);
uintptr_t frame_fp(register_frame_t* regs);
uintptr_t frame_sp(register_frame_t* regs);
void emit_registers_harness(register_frame_t* regs, const uint64_t cr[4]);
void emit_registers_prose(register_frame_t* regs, const uint64_t cr[4]);
}  // namespace arch

struct fp_walk_result {
    static constexpr size_t max_frames = 32;
    size_t depth                       = 0;
    uintptr_t frames[max_frames]       = {};
};

// Frame-pointer backtrace, bounded and fault-safe: every slot is probed
// before it is read. Shared across arches via the frame-slot constants above.
fp_walk_result walk_frame_pointers(uintptr_t start_fp);

enum class trigger_kind : unsigned {
    panic     = 1,
    exception = 2,
    assertion = 3,
    watchdog  = 4,
};

[[noreturn]] void dispatch(trigger_kind kind, register_frame_t* regs, const char* message = nullptr,
                           const char* file = nullptr, int line = 0);

void set_harness_enabled(bool enabled);
void set_test_name(const char* name);

void crash_write(const char* s);

// Scratch buffer behind crash_emit; crash context is single-threaded.
char* crash_fmt_buf(size_t& size);

// Format-and-write for crash output; shared by the core dumper and the
// per-arch register renderers.
template <typename... Args> void crash_emit(const char* fmt, const Args&... args) {
    size_t size;
    char* buf = crash_fmt_buf(size);
    ktl::format::format_to_buffer_raw(buf, size, fmt, args...);
    crash_write(buf);
}

}  // namespace kernel::crash
