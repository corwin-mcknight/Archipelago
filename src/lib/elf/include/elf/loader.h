#pragma once

#include <elf/format.h>
#include <stddef.h>
#include <stdint.h>

// Userspace static ELF loader. The pure parser is also exercised by host tests:
//
//   parse_image() is pure -- no allocation, no globals, no VMM -- because it does the offset and
//   size arithmetic over attacker-controlled header fields, which is where an over-read would live.
//   The host tier unit-tests it and the fuzz lane drives it on arbitrary bytes.
//
//   spawn() prepares VMOs and calls the capability-gated task-construction syscalls.
//
// Accept set: static ET_EXEC for this machine. PT_LOAD is mapped, PT_INTERP and PT_DYNAMIC are
// rejected rather than ignored (a dynamically linked binary run without its interpreter would fail
// far from the cause), and everything else is skipped. Nonempty PT_LOADs must be readable and obey W^X
// (R, RW, or RX); the entry point must lie in an executable segment.

namespace elf {

// Why an image was refused. Distinct per rejection because parse_image() is pure and cannot log:
// the reason has to travel out in the return value or it is lost, and "it failed" is not a
// diagnostic anyone can act on.
enum class elf_error {
    none,
    not_elf,                  // too small, misaligned, bad magic, wrong class/encoding/version
    wrong_machine,            // built for another architecture
    not_executable,           // not ET_EXEC (ET_DYN, ET_REL, ET_CORE)
    dynamic,                  // carries PT_INTERP or PT_DYNAMIC
    truncated,                // a header or segment extends past the end of the image
    bad_entry,                // entry point outside every executable segment
    no_segments,              // nothing to load
    too_many_segments,        // more PT_LOADs than MAX_SEGMENTS
    wx_segment,               // writable and executable at once
    unsupported_permissions,  // nonempty PT_LOAD without PF_R
    unaligned_segment,        // p_vaddr is not page-aligned
    bad_segment,              // p_filesz > p_memsz, or the mapping would wrap
    image_too_large,          // mapped extent exceeds MAX_IMAGE_BYTES
};

const char* to_string(elf_error error);

// One PT_LOAD, validated. `flags` stays in the ELF domain (PF_R/PF_W/PF_X) so the parser owes
// nothing to the VMM; spawn() translates to mapping permissions.
struct segment {
    uint64_t file_offset;
    uint64_t vaddr;
    uint64_t filesz;
    uint64_t memsz;
    uint32_t flags;
};

// A small fixed cap: real static binaries carry a handful of PT_LOADs, and a fixed array keeps
// parse_image() allocation-free and therefore usable from the fuzz lane.
constexpr size_t MAX_SEGMENTS      = 8;

// Ceiling on the summed page-rounded p_memsz of every PT_LOAD. p_memsz is not bounded by the file
// -- a few-KB image may declare gigabytes of .bss -- and spawn() prepares VMOs
// from it, so without a ceiling a small binary decides how much of the machine's memory to consume.
constexpr uint64_t MAX_IMAGE_BYTES = 64ull * 1024 * 1024;

struct image {
    segment segments[MAX_SEGMENTS];
    size_t count;
    uint64_t entry;
};

// Pure parser result, independent of kernel types and allocation.
struct ParseResult {
    image value;
    elf_error error;
    bool is_ok() const { return error == elf_error::none; }
    bool is_err() const { return !is_ok(); }
    image unwrap() const { return value; }
    elf_error unwrap_err() const { return error; }
};
ParseResult parse_image(const void* data, size_t size);

// Loads a static ELF using task-construction syscalls. Borrows the image handle, consumes neither
// it nor the factory. Scratch IPC offsets 0..159 are overwritten. Success writes task/mailbox
// handles at output_offset. Failure aborts this call's unfinished construction.
uint64_t spawn(uint64_t factory, uint64_t image_handle, uint64_t image_size, const char* name, size_t name_size,
               uint64_t output_offset, char* ipc = nullptr);

}  // namespace elf
