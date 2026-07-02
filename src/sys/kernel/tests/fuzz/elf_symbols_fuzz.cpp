// libFuzzer target for the ELF symbol-table locator (host fuzz lane, step 6).
//
// locate_symbol_tables validates an ELF64 blob and returns pointers to its .symtab and string table.
// It is pure (no global state) and does heavy offset/count/index arithmetic on attacker-controlled
// header fields (e_shoff, e_shnum, sh_link, sh_offset/size, sh_entsize) -- the classic place an
// over-read hides. The kernel ingests this blob at boot, so it is a real untrusted-input boundary.
//
// The blob lives in an exactly-sized heap allocation (no slack) so any over-read during parsing is a
// precise ASan report. On a successful parse we then read every byte of the returned syms/strtab
// regions: that is an oracle for the parser's core promise -- the regions it hands back must lie
// fully inside the input. A dishonest bound trips ASan here.
//
// Freestanding like the code under test: malloc/free are declared here and satisfied by libc at link.

#include <kernel/elf_symbols.h>
#include <stddef.h>
#include <stdint.h>

extern "C" void* malloc(size_t);
extern "C" void free(void*);

// KTL bounds checks reach for the global panic(). During fuzzing a panic IS a finding, so trap.
void panic(const char*) { __builtin_trap(); }

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    uint8_t* blob = static_cast<uint8_t*>(malloc(size));  // exact size: over-read hits the redzone
    for (size_t i = 0; i < size; i++) { blob[i] = data[i]; }

    auto tables = kernel::symbols::detail::locate_symbol_tables(blob, size);
    if (tables) {
        volatile uint64_t sink = 0;
        for (size_t i = 0; i < tables->count; i++) {
            const auto& s = tables->syms[i];  // must be in-bounds if count/region math is honest
            sink += s.st_name + s.st_info + s.st_value + s.st_size;
        }
        for (size_t i = 0; i < tables->strtab_size; i++) { sink += static_cast<unsigned char>(tables->strtab[i]); }
        (void)sink;
    }

    free(blob);
    return 0;
}
