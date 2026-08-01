#pragma once

#include <kernel/elf.h>
#include <stddef.h>
#include <stdint.h>

#include <ktl/result>

namespace kernel::mm { class vm_aspace; }

// The kernel's one built-in binary loader. Split in two halves on purpose:
//
//   parse_image() is pure -- no allocation, no globals, no VMM -- because it does the offset and
//   size arithmetic over attacker-controlled header fields, which is where an over-read would live.
//   The host tier unit-tests it and the fuzz lane drives it on arbitrary bytes.
//
//   map_image() turns a parsed image into VMOs and bindings, and can only fail for resource reasons.
//
// Accept set: static ET_EXEC for this machine. PT_LOAD is mapped, PT_INTERP and PT_DYNAMIC are
// rejected rather than ignored (a dynamically linked binary run without its interpreter would fail
// far from the cause), and everything else is skipped.

namespace kernel::elf {

// Why an image was refused. Distinct per rejection because parse_image() is pure and cannot log:
// the reason has to travel out in the return value or it is lost, and "it failed" is not a
// diagnostic anyone can act on.
enum class elf_error {
    not_elf,            // too small, misaligned, bad magic, wrong class/encoding/version
    wrong_machine,      // built for another architecture
    not_executable,     // not ET_EXEC (ET_DYN, ET_REL, ET_CORE)
    dynamic,            // carries PT_INTERP or PT_DYNAMIC
    truncated,          // a header or segment extends past the end of the image
    bad_entry,          // entry point outside every loadable segment
    no_segments,        // nothing to load
    too_many_segments,  // more PT_LOADs than MAX_SEGMENTS
    wx_segment,         // writable and executable at once
    unaligned_segment,  // p_vaddr is not page-aligned
    bad_segment,        // p_filesz > p_memsz, or the mapping would wrap
};

const char* to_string(elf_error error);

// One PT_LOAD, validated. `flags` stays in the ELF domain (PF_R/PF_W/PF_X) so the parser owes
// nothing to the VMM; map_image() translates to vm_prot.
struct segment {
    uint64_t file_offset;
    uint64_t vaddr;
    uint64_t filesz;
    uint64_t memsz;
    uint32_t flags;
};

// A small fixed cap: real static binaries carry a handful of PT_LOADs, and a fixed array keeps
// parse_image() allocation-free and therefore usable from the fuzz lane.
constexpr size_t MAX_SEGMENTS = 8;

struct image {
    segment segments[MAX_SEGMENTS];
    size_t count;
    uint64_t entry;
};

// Validate `data` and collect its loadable segments. Pure: touches no global state.
ktl::result<image, elf_error> parse_image(const void* data, size_t size);

// Back each segment with an anonymous VMO and bind it into `aspace`. Segment bytes are copied
// through the physmap, so no address-space switch is needed; the bytes past p_filesz are already
// zero because anonymous VMOs zero-fill, which is what covers .bss.
//
// Range and overlap are not re-checked here: Region::map already rejects a binding outside the root
// region or overlapping a sibling, and a partially mapped image is not a leak because the caller
// discards the whole address space on failure.
ktl::result<void> map_image(kernel::mm::vm_aspace& aspace, const void* data, size_t size, const image& img);

}  // namespace kernel::elf
