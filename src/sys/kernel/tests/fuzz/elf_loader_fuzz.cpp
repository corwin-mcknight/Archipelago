// libFuzzer target for the user-binary ELF loader's parse half (host fuzz lane).
//
// parse_image() validates an ELF64 image and returns the segments the kernel will map. It is pure
// (no global state, no allocation) and does offset/size arithmetic on entirely attacker-controlled
// header fields (e_phoff, e_phnum, e_phentsize, p_offset, p_filesz, p_memsz, p_vaddr) -- the classic
// place an over-read hides. Once a filesystem replaces the boot module, this is the kernel's real
// untrusted-input boundary for executable code.
//
// The blob lives in an exactly-sized heap allocation (no slack) so any over-read during parsing is a
// precise ASan report. On a successful parse we then read every file byte each returned segment
// claims: that is an oracle for the parser's core promise -- a segment it accepts must lie fully
// inside the input, because map_image() copies exactly that range.
//
// Freestanding like the code under test: malloc/free are declared here and satisfied by libc at link.

#include <kernel/elf_loader.h>
#include <stddef.h>
#include <stdint.h>

extern "C" void* malloc(size_t);
extern "C" void free(void*);

// KTL bounds checks reach for the global panic(). During fuzzing a panic IS a finding, so trap.
void panic(const char*) { __builtin_trap(); }

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    uint8_t* blob = static_cast<uint8_t*>(malloc(size));  // exact size: over-read hits the redzone
    for (size_t i = 0; i < size; i++) { blob[i] = data[i]; }

    auto parsed = kernel::elf::parse_image(blob, size);
    if (parsed.is_ok()) {
        auto img = parsed.unwrap();
        if (img.count > kernel::elf::MAX_SEGMENTS) { __builtin_trap(); }

        volatile uint64_t sink = 0;
        for (size_t i = 0; i < img.count; i++) {
            const auto& seg = img.segments[i];
            if (seg.filesz > seg.memsz) { __builtin_trap(); }
            // Exactly what map_image() copies; a dishonest bound trips ASan here.
            for (uint64_t off = 0; off < seg.filesz; off++) { sink += blob[seg.file_offset + off]; }
        }
        (void)sink;
    }

    free(blob);
    return 0;
}
