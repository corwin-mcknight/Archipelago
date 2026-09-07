#include <elf/loader.h>

// The pure half of the loader. Deliberately its own translation unit: it pulls in no VMM and no
// hardware, so the host test runner and the fuzz lane can link it directly.

namespace elf {

namespace {

constexpr uint64_t PAGE_SIZE = 4096;

uint64_t mapped_bytes(uint64_t memsz) { return (memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1); }
bool extent_wraps(uint64_t vaddr, uint64_t memsz) {
    return memsz > UINT64_MAX - (PAGE_SIZE - 1) || mapped_bytes(memsz) > UINT64_MAX - vaddr;
}
ParseResult failure(elf_error error) { return {{}, error}; }

elf_error check_segment(const Elf64_Phdr& ph, size_t size) {
    if ((ph.p_flags & PF_W) != 0 && (ph.p_flags & PF_X) != 0) { return elf_error::wx_segment; }
    // Require readable mappings on both targets: x86 cannot remove read access, and RISC-V
    // forbids write-only leaf PTEs. Along with W^X, the supported sets are R, RW, and RX.
    if ((ph.p_flags & PF_R) == 0) { return elf_error::unsupported_permissions; }
    if (ph.p_vaddr % PAGE_SIZE != 0) { return elf_error::unaligned_segment; }
    if (ph.p_filesz > ph.p_memsz) { return elf_error::bad_segment; }
    if (extent_wraps(ph.p_vaddr, ph.p_memsz)) { return elf_error::bad_segment; }
    if (!region_in_bounds(ph.p_offset, ph.p_filesz, size)) { return elf_error::truncated; }
    return elf_error::none;
}

// The entry point must land inside the declared extent of an executable mapping.
bool entry_covered(const image& img) {
    for (size_t i = 0; i < img.count; i++) {
        const segment& seg = img.segments[i];
        if ((seg.flags & PF_X) != 0 && img.entry >= seg.vaddr && img.entry - seg.vaddr < seg.memsz) { return true; }
    }
    return false;
}

}  // namespace

const char* to_string(elf_error error) {
    switch (error) {
        case elf_error::none: return "ok";
        case elf_error::not_elf: return "not an ELF64 image";
        case elf_error::wrong_machine: return "built for another architecture";
        case elf_error::not_executable: return "not a static executable";
        case elf_error::dynamic: return "dynamically linked";
        case elf_error::truncated: return "truncated";
        case elf_error::bad_entry: return "entry point outside every executable segment";
        case elf_error::no_segments: return "no loadable segments";
        case elf_error::too_many_segments: return "too many loadable segments";
        case elf_error::wx_segment: return "segment is writable and executable";
        case elf_error::unsupported_permissions: return "segment is not readable";
        case elf_error::unaligned_segment: return "segment is not page-aligned";
        case elf_error::bad_segment: return "malformed segment";
        case elf_error::image_too_large: return "mapped image exceeds the size ceiling";
        default: return "?";
    }
}

ParseResult parse_image(const void* data, size_t size) {
    const Elf64_Ehdr* hdr = header_of(data, size);
    if (hdr == nullptr) { return failure(elf_error::not_elf); }
    if (hdr->e_machine != EM_NATIVE) { return failure(elf_error::wrong_machine); }
    if (hdr->e_type != ET_EXEC) { return failure(elf_error::not_executable); }
    if (hdr->e_phoff == 0 || hdr->e_phentsize < sizeof(Elf64_Phdr)) { return failure(elf_error::truncated); }

    uint64_t table_bytes = static_cast<uint64_t>(hdr->e_phnum) * hdr->e_phentsize;
    if (!region_in_bounds(hdr->e_phoff, table_bytes, size)) { return failure(elf_error::truncated); }

    const auto* base = static_cast<const uint8_t*>(data);
    // Validate both the table start and stride so every Elf64_Phdr read is aligned.
    if (reinterpret_cast<uintptr_t>(base + hdr->e_phoff) % alignof(Elf64_Phdr) != 0 ||
        hdr->e_phentsize % alignof(Elf64_Phdr) != 0) {
        return failure(elf_error::truncated);
    }

    image img      = {};
    img.entry      = hdr->e_entry;
    uint64_t total = 0;

    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        const auto& ph =
            *reinterpret_cast<const Elf64_Phdr*>(base + hdr->e_phoff + static_cast<uint64_t>(i) * hdr->e_phentsize);
        if (ph.p_type == PT_INTERP || ph.p_type == PT_DYNAMIC) { return failure(elf_error::dynamic); }
        if (ph.p_type != PT_LOAD) { continue; }
        if (ph.p_filesz > ph.p_memsz) { return failure(elf_error::bad_segment); }
        if (ph.p_memsz == 0) { continue; }

        auto checked = check_segment(ph, size);
        if (checked != elf_error::none) { return failure(checked); }
        if (img.count == MAX_SEGMENTS) { return failure(elf_error::too_many_segments); }

        // check_segment already proved the rounding does not overflow, and each addend is capped
        // before it is added, so the running total cannot wrap.
        uint64_t bytes = mapped_bytes(ph.p_memsz);
        if (bytes > MAX_IMAGE_BYTES) { return failure(elf_error::image_too_large); }
        total += bytes;
        if (total > MAX_IMAGE_BYTES) { return failure(elf_error::image_too_large); }

        img.segments[img.count++] = {
            .file_offset = ph.p_offset,
            .vaddr       = ph.p_vaddr,
            .filesz      = ph.p_filesz,
            .memsz       = ph.p_memsz,
            .flags       = ph.p_flags,
        };
    }

    if (img.count == 0) { return failure(elf_error::no_segments); }
    if (!entry_covered(img)) { return failure(elf_error::bad_entry); }
    return {img, elf_error::none};
}

}  // namespace elf
