#include <kernel/elf_loader.h>
#include <kernel/testing/testing.h>

using namespace kernel::elf;

KTEST_MODULE("kernel/elf_loader");

namespace {

// A synthetic ELF64 image, valid by default, that each case damages in exactly one way. Building
// images by hand rather than by linking one keeps the malformed cases reachable -- a linker will not
// emit a segment whose p_filesz exceeds its p_memsz, but an attacker will.
//
// alignas matters: parse_image() rejects a misaligned image outright, so an under-aligned buffer
// would make every case fail for the wrong reason.
struct Image {
    static constexpr size_t PHOFF            = sizeof(Elf64_Ehdr);
    static constexpr size_t MAX_PH           = 16;
    static constexpr size_t DATA_OFF         = PHOFF + MAX_PH * sizeof(Elf64_Phdr);

    alignas(8) uint8_t bytes[DATA_OFF + 512] = {};
    size_t size                              = sizeof(bytes);

    explicit Image(uint16_t phnum = 1) {
        auto& h = header();
        for (size_t i = 0; i < sizeof(ELF_MAGIC); i++) { h.e_ident[i] = ELF_MAGIC[i]; }
        h.e_ident[EI_CLASS]   = ELFCLASS64;
        h.e_ident[EI_DATA]    = ELFDATA2LSB;
        h.e_ident[EI_VERSION] = EV_CURRENT;
        h.e_type              = ET_EXEC;
        h.e_machine           = EM_NATIVE;
        h.e_version           = EV_CURRENT;
        h.e_entry             = 0x400000;
        h.e_phoff             = PHOFF;
        h.e_phentsize         = sizeof(Elf64_Phdr);
        h.e_phnum             = phnum;
        h.e_ehsize            = sizeof(Elf64_Ehdr);

        for (uint16_t i = 0; i < phnum; i++) { set_load(i, 0x400000 + static_cast<uint64_t>(i) * 0x1000); }
    }

    Elf64_Ehdr& header() { return *reinterpret_cast<Elf64_Ehdr*>(bytes); }
    Elf64_Phdr& phdr(size_t i) { return reinterpret_cast<Elf64_Phdr*>(bytes + PHOFF)[i]; }

    void set_load(size_t i, uint64_t vaddr) {
        auto& p    = phdr(i);
        p.p_type   = PT_LOAD;
        p.p_flags  = PF_R | PF_X;
        p.p_offset = DATA_OFF;
        p.p_vaddr  = vaddr;
        p.p_filesz = 0x40;
        p.p_memsz  = 0x1000;
        p.p_align  = 0x1000;
    }

    ktl::result<image, elf_error> parse() { return parse_image(bytes, size); }
};

// Asserts the specific reason, not merely that parsing failed: a case that starts failing for a
// different reason than intended has stopped testing what it claims to.
bool rejects_with(Image& img, elf_error expected) {
    auto parsed = img.parse();
    return parsed.is_err() && parsed.unwrap_err() == expected;
}

template <typename F> bool rejects_with(elf_error expected, F mutate, uint16_t phnum = 1) {
    Image img(phnum);
    mutate(img);
    return rejects_with(img, expected);
}

}  // namespace

KTEST_CASE(elf_loader_accepts_a_well_formed_image) {
    Image img(2);
    img.phdr(1).p_flags = PF_R | PF_W;

    auto parsed         = img.parse();
    KTEST_REQUIRE_TRUE(parsed.is_ok());

    auto loaded = parsed.unwrap();
    KTEST_REQUIRE_EQUAL(loaded.count, 2u);
    KTEST_EXPECT_EQUAL(loaded.entry, 0x400000ull);
    KTEST_EXPECT_EQUAL(loaded.segments[0].vaddr, 0x400000ull);
    KTEST_EXPECT_EQUAL(loaded.segments[0].filesz, 0x40ull);
    KTEST_EXPECT_EQUAL(loaded.segments[0].memsz, 0x1000ull);
    KTEST_EXPECT_EQUAL(loaded.segments[1].vaddr, 0x401000ull);
    KTEST_EXPECT_ALL((loaded.segments[0].flags & PF_X) != 0, (loaded.segments[1].flags & PF_W) != 0);
}

KTEST_CASE(elf_loader_rejects_malformed_headers) {
    KTEST_EXPECT_TRUE(rejects_with(elf_error::not_elf, [](Image& i) { i.header().e_ident[1] = 'X'; }));

    // ELFCLASS32
    KTEST_EXPECT_TRUE(rejects_with(elf_error::not_elf, [](Image& i) { i.header().e_ident[EI_CLASS] = 1; }));

    // ELFDATA2MSB
    KTEST_EXPECT_TRUE(rejects_with(elf_error::not_elf, [](Image& i) { i.header().e_ident[EI_DATA] = 2; }));

    // Shorter than a header at all: the size check must come before any field is read.
    Image runt;
    runt.size = sizeof(Elf64_Ehdr) - 1;
    KTEST_EXPECT_TRUE(rejects_with(runt, elf_error::not_elf));

    KTEST_EXPECT_ERR(parse_image(nullptr, 4096), elf_error::not_elf);
}

KTEST_CASE(elf_loader_rejects_images_it_cannot_run) {
    Image wrong_arch;
    wrong_arch.header().e_machine = EM_NATIVE == EM_RISCV ? EM_X86_64 : EM_RISCV;
    KTEST_EXPECT_TRUE(rejects_with(wrong_arch, elf_error::wrong_machine));

    KTEST_EXPECT_TRUE(rejects_with(elf_error::not_executable, [](Image& i) { i.header().e_type = ET_DYN; }));

    KTEST_EXPECT_TRUE(rejects_with(elf_error::not_executable, [](Image& i) { i.header().e_type = ET_REL; }));

    // A dynamically linked binary is refused rather than loaded without its interpreter, which
    // would fail far away from the cause.
    KTEST_EXPECT_TRUE(rejects_with(elf_error::dynamic, [](Image& i) { i.phdr(1).p_type = PT_INTERP; }, 2));

    KTEST_EXPECT_TRUE(rejects_with(elf_error::dynamic, [](Image& i) { i.phdr(1).p_type = PT_DYNAMIC; }, 2));
}

KTEST_CASE(elf_loader_enforces_segment_policy) {
    Image wx;
    wx.phdr(0).p_flags = PF_R | PF_W | PF_X;
    KTEST_EXPECT_TRUE(rejects_with(wx, elf_error::wx_segment));

    Image unaligned;
    unaligned.phdr(0).p_vaddr  = 0x400800;
    unaligned.header().e_entry = 0x400800;
    KTEST_EXPECT_TRUE(rejects_with(unaligned, elf_error::unaligned_segment));

    // More file bytes than memory to put them in.
    Image fat;
    fat.phdr(0).p_filesz = fat.phdr(0).p_memsz + 1;
    KTEST_EXPECT_TRUE(rejects_with(fat, elf_error::bad_segment));

    // An extent that would wrap the address space must not alias low memory.
    Image wrapping;
    wrapping.phdr(0).p_vaddr  = 0xFFFFFFFFFFFFF000ull;
    wrapping.phdr(0).p_memsz  = 0x8000;
    wrapping.header().e_entry = 0xFFFFFFFFFFFFF000ull;
    KTEST_EXPECT_TRUE(rejects_with(wrapping, elf_error::bad_segment));

    // PT_NOTE: skipped, not mapped
    KTEST_EXPECT_TRUE(rejects_with(elf_error::no_segments, [](Image& i) { i.phdr(0).p_type = 4; }));

    // Entry lands in no mapped segment, so the first instruction fetch would fault.
    KTEST_EXPECT_TRUE(rejects_with(elf_error::bad_entry, [](Image& i) { i.header().e_entry = 0x900000; }));
}

KTEST_CASE(elf_loader_caps_mapped_size) {
    // p_memsz is not bounded by the file, so a small image can ask for an arbitrary mapping. The
    // ceiling has to hold whether one segment is oversized or several add up past it.
    Image huge;
    huge.phdr(0).p_memsz = MAX_IMAGE_BYTES + 0x1000;
    KTEST_EXPECT_TRUE(rejects_with(huge, elf_error::image_too_large));

    // Neither segment breaches the ceiling alone; together they do.
    Image summed(2);
    summed.phdr(0).p_memsz = MAX_IMAGE_BYTES - 0x1000;
    summed.phdr(1).p_memsz = MAX_IMAGE_BYTES - 0x1000;
    summed.phdr(1).p_vaddr = summed.phdr(0).p_vaddr + MAX_IMAGE_BYTES;
    KTEST_EXPECT_TRUE(rejects_with(summed, elf_error::image_too_large));

    // A segment right at the ceiling still loads.
    Image at_limit;
    at_limit.phdr(0).p_memsz = MAX_IMAGE_BYTES;
    KTEST_EXPECT_TRUE(at_limit.parse().is_ok());
}

KTEST_CASE(elf_loader_rejects_out_of_bounds_offsets) {
    // Program header table claims to start past the end of the image.
    KTEST_EXPECT_TRUE(rejects_with(elf_error::truncated, [](Image& i) { i.header().e_phoff = i.size + 0x1000; }));

    // Table starts inside but its declared extent runs off the end.
    KTEST_EXPECT_TRUE(rejects_with(elf_error::truncated, [](Image& i) { i.header().e_phnum = 4096; }));

    // Segment's file range runs past the end -- the case that would become an over-read in the copy.
    Image far_segment;
    far_segment.phdr(0).p_offset = far_segment.size - 8;
    far_segment.phdr(0).p_filesz = 0x100;
    KTEST_EXPECT_TRUE(rejects_with(far_segment, elf_error::truncated));

    // Offset arithmetic must not wrap into a range that looks in-bounds.
    Image wrapping_offset;
    wrapping_offset.phdr(0).p_offset = 0xFFFFFFFFFFFFFFF0ull;
    wrapping_offset.phdr(0).p_filesz = 0x20;
    KTEST_EXPECT_TRUE(rejects_with(wrapping_offset, elf_error::truncated));

    Image tiny_entsize;
    tiny_entsize.header().e_phentsize = sizeof(Elf64_Phdr) - 1;
    KTEST_EXPECT_TRUE(rejects_with(tiny_entsize, elf_error::truncated));
}

KTEST_CASE(elf_loader_caps_segment_count) {
    static_assert(MAX_SEGMENTS + 1 <= Image::MAX_PH, "test image needs room for one segment past the cap");

    Image img(1);
    img.header().e_phnum = static_cast<uint16_t>(MAX_SEGMENTS + 1);
    for (size_t i = 1; i < MAX_SEGMENTS + 1; i++) { img.set_load(i, 0x400000 + i * 0x1000); }

    KTEST_EXPECT_TRUE(rejects_with(img, elf_error::too_many_segments));
}
