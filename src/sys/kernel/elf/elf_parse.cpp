#include <kernel/config.h>
#include <kernel/elf_loader.h>

#include <ktl/maybe>

// The pure half of the loader. Deliberately its own translation unit: it pulls in no VMM and no
// hardware, so the host test runner and the fuzz lane can link it directly.

namespace kernel::elf {

namespace {

constexpr uint64_t PAGE_SIZE = KERNEL_MINIMUM_PAGE_SIZE;

// Bytes a segment occupies once mapped, or nothing when rounding overflows.
ktl::maybe<uint64_t> mapped_bytes(uint64_t memsz) {
    uint64_t rounded = memsz + (PAGE_SIZE - 1);
    if (rounded < memsz) { return ktl::nothing; }
    return rounded & ~(PAGE_SIZE - 1);
}

// A segment's mapping is [vaddr, vaddr + round_up(memsz)). Reject anything whose extent would wrap
// the address space rather than letting the arithmetic silently alias low memory.
bool extent_wraps(uint64_t vaddr, uint64_t memsz) {
    auto bytes = mapped_bytes(memsz);
    if (!bytes.has_value()) { return true; }
    return vaddr + bytes.value() < vaddr;
}

ktl::result<void, elf_error> check_segment(const Elf64_Phdr& ph, size_t size) {
    if ((ph.p_flags & PF_W) != 0 && (ph.p_flags & PF_X) != 0) { return ktl::err(elf_error::wx_segment); }
    if (ph.p_vaddr % PAGE_SIZE != 0) { return ktl::err(elf_error::unaligned_segment); }
    if (ph.p_filesz > ph.p_memsz) { return ktl::err(elf_error::bad_segment); }
    if (extent_wraps(ph.p_vaddr, ph.p_memsz)) { return ktl::err(elf_error::bad_segment); }
    if (!region_in_bounds(ph.p_offset, ph.p_filesz, size)) { return ktl::err(elf_error::truncated); }
    return ktl::result<void, elf_error>::ok();
}

// The entry point must land inside something we actually mapped, or the first instruction fetched
// would fault at an address the image never described.
bool entry_covered(const image& img) {
    for (size_t i = 0; i < img.count; i++) {
        const segment& seg = img.segments[i];
        if (img.entry >= seg.vaddr && img.entry - seg.vaddr < seg.memsz) { return true; }
    }
    return false;
}

}  // namespace

const char* to_string(elf_error error) {
    switch (error) {
        case elf_error::not_elf: return "not an ELF64 image";
        case elf_error::wrong_machine: return "built for another architecture";
        case elf_error::not_executable: return "not a static executable";
        case elf_error::dynamic: return "dynamically linked";
        case elf_error::truncated: return "truncated";
        case elf_error::bad_entry: return "entry point outside every segment";
        case elf_error::no_segments: return "no loadable segments";
        case elf_error::too_many_segments: return "too many loadable segments";
        case elf_error::wx_segment: return "segment is writable and executable";
        case elf_error::unaligned_segment: return "segment is not page-aligned";
        case elf_error::bad_segment: return "malformed segment";
        case elf_error::image_too_large: return "mapped image exceeds the size ceiling";
        default: return "?";
    }
}

ktl::result<image, elf_error> parse_image(const void* data, size_t size) {
    const Elf64_Ehdr* hdr = header_of(data, size);
    if (hdr == nullptr) { return ktl::err(elf_error::not_elf); }
    if (hdr->e_machine != EM_NATIVE) { return ktl::err(elf_error::wrong_machine); }
    if (hdr->e_type != ET_EXEC) { return ktl::err(elf_error::not_executable); }
    if (hdr->e_phoff == 0 || hdr->e_phentsize < sizeof(Elf64_Phdr)) { return ktl::err(elf_error::truncated); }

    uint64_t table_bytes = static_cast<uint64_t>(hdr->e_phnum) * hdr->e_phentsize;
    if (!region_in_bounds(hdr->e_phoff, table_bytes, size)) { return ktl::err(elf_error::truncated); }

    const auto* base = static_cast<const uint8_t*>(data);
    // Same reason as the header's own alignment check: these are read as Elf64_Phdr structs.
    if ((reinterpret_cast<uintptr_t>(base) + hdr->e_phoff) % alignof(Elf64_Phdr) != 0) {
        return ktl::err(elf_error::truncated);
    }

    image img      = {};
    img.entry      = hdr->e_entry;
    uint64_t total = 0;

    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        const auto& ph = *reinterpret_cast<const Elf64_Phdr*>(base + hdr->e_phoff + i * hdr->e_phentsize);
        if (ph.p_type == PT_INTERP || ph.p_type == PT_DYNAMIC) { return ktl::err(elf_error::dynamic); }
        if (ph.p_type != PT_LOAD) { continue; }
        if (ph.p_memsz == 0) { continue; }

        auto checked = check_segment(ph, size);
        if (checked.is_err()) { return ktl::err(checked.unwrap_err()); }
        if (img.count == MAX_SEGMENTS) { return ktl::err(elf_error::too_many_segments); }

        // check_segment already proved the rounding does not overflow, and each addend is capped
        // before it is added, so the running total cannot wrap.
        uint64_t bytes = mapped_bytes(ph.p_memsz).value();
        if (bytes > MAX_IMAGE_BYTES) { return ktl::err(elf_error::image_too_large); }
        total += bytes;
        if (total > MAX_IMAGE_BYTES) { return ktl::err(elf_error::image_too_large); }

        img.segments[img.count++] = {
            .file_offset = ph.p_offset,
            .vaddr       = ph.p_vaddr,
            .filesz      = ph.p_filesz,
            .memsz       = ph.p_memsz,
            .flags       = ph.p_flags,
        };
    }

    if (img.count == 0) { return ktl::err(elf_error::no_segments); }
    if (!entry_covered(img)) { return ktl::err(elf_error::bad_entry); }
    return ktl::result<image, elf_error>::ok(img);
}

}  // namespace kernel::elf
