#include <kernel/elf_symbols.h>
#include <kernel/symbols.h>
#include <kernel/testing/testing.h>

using namespace kernel::elf;

KTEST_MODULE("kernel/elf_symbols");

namespace {

struct SymbolImage {
    Elf64_Ehdr header      = {};
    Elf64_Shdr sections[3] = {};
    Elf64_Sym syms[3]      = {};
    char strings[32]       = "\0small\0large\0high";

    SymbolImage() {
        for (size_t i = 0; i < sizeof(ELF_MAGIC); i++) { header.e_ident[i] = ELF_MAGIC[i]; }
        header.e_ident[EI_CLASS]   = ELFCLASS64;
        header.e_ident[EI_DATA]    = ELFDATA2LSB;
        header.e_ident[EI_VERSION] = EV_CURRENT;
        header.e_shoff             = offsetof(SymbolImage, sections);
        header.e_shentsize         = sizeof(Elf64_Shdr);
        header.e_shnum             = 3;
        sections[1].sh_type        = SHT_SYMTAB;
        sections[1].sh_offset      = offsetof(SymbolImage, syms);
        sections[1].sh_size        = sizeof(syms);
        sections[1].sh_entsize     = sizeof(Elf64_Sym);
        sections[1].sh_link        = 2;
        sections[2].sh_type        = 3;  // SHT_STRTAB
        sections[2].sh_offset      = offsetof(SymbolImage, strings);
        sections[2].sh_size        = sizeof(strings);
        syms[0]                    = {.st_name = 1, .st_info = STT_FUNC, .st_value = 0x1000, .st_size = 0x10};
        syms[1]                    = {.st_name = 7, .st_info = STT_FUNC, .st_value = 0x2000, .st_size = 0x100000010ull};
        syms[2]                    = {.st_name = 13, .st_info = STT_FUNC, .st_value = UINT64_MAX - 15, .st_size = 16};
    }

    auto locate() { return kernel::symbols::detail::locate_symbol_tables(this, sizeof(*this)); }
};

}  // namespace

KTEST_CASE(elf_symbols_locates_exact_tables) {
    SymbolImage img;
    auto tables = img.locate();
    KTEST_REQUIRE_TRUE(tables.has_value());
    KTEST_EXPECT_TRUE(tables->syms == img.syms);
    KTEST_EXPECT_EQUAL(tables->count, 3u);
    KTEST_EXPECT_TRUE(tables->strtab == img.strings);
    KTEST_EXPECT_EQUAL(tables->strtab_size, sizeof(img.strings));
}

KTEST_CASE(elf_symbols_rejects_inconsistent_strides) {
    constexpr uint16_t section_strides[] = {0, 63, 65, 72};
    for (uint16_t stride : section_strides) {
        SymbolImage img;
        img.header.e_shentsize = stride;
        KTEST_EXPECT_FALSE(img.locate().has_value());
    }
    constexpr uint64_t symbol_strides[] = {0, 23, 25, 32};
    for (uint64_t stride : symbol_strides) {
        SymbolImage img;
        img.sections[1].sh_entsize = stride;
        KTEST_EXPECT_FALSE(img.locate().has_value());
    }
    SymbolImage partial;
    partial.sections[1].sh_size--;
    KTEST_EXPECT_FALSE(partial.locate().has_value());
}

KTEST_CASE(elf_symbols_rejects_misaligned_and_truncated_tables) {
    SymbolImage sections;
    sections.header.e_shoff++;
    KTEST_EXPECT_FALSE(sections.locate().has_value());
    SymbolImage syms;
    syms.sections[1].sh_offset++;
    KTEST_EXPECT_FALSE(syms.locate().has_value());
    SymbolImage truncated;
    truncated.sections[1].sh_offset = sizeof(truncated) - sizeof(Elf64_Sym);
    KTEST_EXPECT_FALSE(truncated.locate().has_value());
    SymbolImage link;
    link.sections[1].sh_link = link.header.e_shnum;
    KTEST_EXPECT_FALSE(link.locate().has_value());
}

KTEST_CASE(elf_symbols_preserves_full_width_extents) {
    // Host tests fork before each case, keeping symbol ingestion's one-time global state isolated.
    SymbolImage img;
    kernel::symbols::init(&img, sizeof(img));
    KTEST_REQUIRE_TRUE(kernel::symbols::available());
    KTEST_EXPECT_FALSE(kernel::symbols::lookup(0xfff).has_value());
    KTEST_EXPECT_TRUE(kernel::symbols::lookup(0x100f).has_value());
    KTEST_EXPECT_FALSE(kernel::symbols::lookup(0x1010).has_value());
    auto large = kernel::symbols::lookup(0x10000200full);
    KTEST_REQUIRE_TRUE(large.has_value());
    KTEST_EXPECT_EQUAL(large->offset, 0x10000000full);
    KTEST_EXPECT_FALSE(kernel::symbols::lookup(0x100002010ull).has_value());
    auto high = kernel::symbols::lookup(UINT64_MAX);
    KTEST_REQUIRE_TRUE(high.has_value());
    KTEST_EXPECT_EQUAL(high->offset, 15u);
    KTEST_EXPECT_FALSE(kernel::symbols::lookup(0).has_value());
}
