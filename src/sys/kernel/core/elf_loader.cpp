#include <kernel/config.h>
#include <kernel/elf_loader.h>
#include <kernel/mm/vm_aspace.h>
#include <kernel/mm/vmo.h>
#include <string.h>

extern uintptr_t g_hhdm_offset;

namespace kernel::elf {

namespace {

constexpr uint64_t PAGE_SIZE = KERNEL_MINIMUM_PAGE_SIZE;

size_t pages_for(uint64_t bytes) { return static_cast<size_t>((bytes + PAGE_SIZE - 1) / PAGE_SIZE); }

kernel::mm::vm_prot_t prot_of(uint32_t flags) {
    kernel::mm::vm_prot_t prot = kernel::mm::vm_prot::USER;
    if ((flags & PF_R) != 0) { prot |= kernel::mm::vm_prot::READ; }
    if ((flags & PF_W) != 0) { prot |= kernel::mm::vm_prot::WRITE; }
    if ((flags & PF_X) != 0) { prot |= kernel::mm::vm_prot::EXECUTE; }
    return prot;
}

// Copy a segment's file bytes into its freshly committed frames through the physmap. Pages past
// p_filesz are left untouched: anonymous VMOs zero-fill, which is exactly what .bss needs, and it is
// also what zeroes the tail of the last partially filled page.
ktl::result<void> fill_segment(kernel::mm::vmo& backing, const uint8_t* bytes, uint64_t filesz) {
    for (size_t page = 0; page < pages_for(filesz); page++) {
        auto frame = backing.resident_frame(page);
        if (!frame.has_value()) { return ktl::err(ktl::errc::oom); }

        uint64_t offset = page * PAGE_SIZE;
        uint64_t chunk  = filesz - offset < PAGE_SIZE ? filesz - offset : PAGE_SIZE;
        memcpy(reinterpret_cast<void*>(frame.value() + g_hhdm_offset), bytes + offset, chunk);
    }
    return ktl::result<void>::ok();
}

}  // namespace

ktl::result<void> map_image(kernel::mm::vm_aspace& aspace, const void* data, size_t size, const image& img) {
    const auto* base = static_cast<const uint8_t*>(data);

    for (size_t i = 0; i < img.count; i++) {
        const segment& seg = img.segments[i];
        // parse_image() bounds-checked [file_offset, file_offset + filesz) against the image, so the
        // copy below cannot over-read; assert the contract rather than trusting it silently.
        if (!region_in_bounds(seg.file_offset, seg.filesz, size)) { return ktl::err(ktl::errc::out_of_range); }

        size_t pages = pages_for(seg.memsz);
        auto backing = kernel::mm::create_anonymous_vmo(pages);
        if (!backing) { return ktl::err(ktl::errc::oom); }

        auto committed = backing->commit(0, pages);
        if (committed.is_err()) { return ktl::err(committed.unwrap_err()); }

        auto filled = fill_segment(*backing, base + seg.file_offset, seg.filesz);
        if (filled.is_err()) { return ktl::err(filled.unwrap_err()); }

        auto mapped =
            aspace.root().map(static_cast<uintptr_t>(seg.vaddr), pages * PAGE_SIZE, backing, 0, prot_of(seg.flags));
        if (mapped.is_err()) { return ktl::err(mapped.unwrap_err()); }
    }

    return ktl::result<void>::ok();
}

}  // namespace kernel::elf
