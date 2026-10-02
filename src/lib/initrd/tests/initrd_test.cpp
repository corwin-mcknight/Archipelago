#include <initrd/initrd.h>
#include <kernel/testing/testing.h>

KTEST_MODULE("userspace/initrd");

namespace {

using initrd::BLOCK_SIZE;
using initrd::Entry;
using initrd::Reader;
using initrd::Result;

void copy_text(uint8_t* dest, const char* text) {
    for (size_t i = 0; text[i] != 0; i++) { dest[i] = static_cast<uint8_t>(text[i]); }
}

bool equal_text(const char* first, const char* second) {
    for (size_t i = 0;; i++) {
        if (first[i] != second[i]) { return false; }
        if (first[i] == 0) { return true; }
    }
}

void write_octal(uint8_t* dest, size_t size, uint64_t value) {
    dest[size - 1] = 0;
    for (size_t i = size - 1; i != 0; i--) {
        dest[i - 1] = static_cast<uint8_t>('0' + value % 8);
        value /= 8;
    }
}

void checksum(uint8_t* header) {
    for (size_t i = 148; i < 156; i++) { header[i] = ' '; }
    uint64_t sum = 0;
    for (size_t i = 0; i < BLOCK_SIZE; i++) { sum += header[i]; }
    write_octal(header + 148, 7, sum);
}

// Synthetic headers let each test reach one malformed condition that a normal tar writer refuses
// to produce. A fixture uses trailing zero blocks matching ordinary tar record padding.
struct Archive {
    uint8_t bytes[8192] = {};
    size_t used         = 0;
    size_t size         = 2 * BLOCK_SIZE;

    uint8_t* add(const char* path, const char* data = "", char type = '0', const char* prefix = "") {
        uint8_t* header = bytes + used;
        copy_text(header, path);
        write_octal(header + 100, 8, 0755);
        write_octal(header + 108, 8, 0);
        write_octal(header + 116, 8, 0);
        size_t data_size = 0;
        while (data[data_size] != 0) { data_size++; }
        write_octal(header + 124, 12, data_size);
        write_octal(header + 136, 12, 0);
        header[156] = static_cast<uint8_t>(type);
        copy_text(header + 257, "ustar");
        copy_text(header + 263, "00");
        copy_text(header + 345, prefix);
        copy_text(header + BLOCK_SIZE, data);
        checksum(header);
        used += BLOCK_SIZE + (data_size + BLOCK_SIZE - 1) / BLOCK_SIZE * BLOCK_SIZE;
        size = used + 2 * BLOCK_SIZE;
        return header;
    }

    bool valid() { return Reader(bytes, size).validate(); }
};

template <typename F> bool rejects(F mutate) {
    Archive archive;
    uint8_t* header = archive.add("bootstrap/elf_loader.elf", "elf bytes");
    mutate(header);
    checksum(header);
    return !archive.valid();
}

bool rejects_path(const char* path) {
    Archive archive;
    archive.add(path, "contents");
    return !archive.valid();
}

bool name_valid(const char* path, bool directory = false) {
    Entry entry     = {};
    entry.directory = directory;
    copy_text(reinterpret_cast<uint8_t*>(entry.path), path);
    char name[initrd::BOOTSTRAP_NAME_MAX + 1];
    return initrd::bootstrap_name(entry, name);
}

}  // namespace

KTEST_CASE(initrd_reads_sorted_files_and_directories) {
    Archive archive;
    archive.add("bootstrap/", "", '5');
    archive.add("bootstrap/echo.elf", "echo bytes");
    archive.add("bootstrap/elf_loader.elf", "loader bytes");
    archive.add("data", "", '5');
    archive.add("data/readme", "hello");
    Reader reader(archive.bytes, archive.size);
    KTEST_REQUIRE_TRUE(reader.validate());
    Entry entry;
    KTEST_REQUIRE_TRUE(reader.next(entry) == Result::ENTRY);
    KTEST_EXPECT_ALL(entry.directory, entry.size == 0, equal_text(entry.path, "bootstrap"));
    KTEST_EXPECT_FALSE(initrd::is_bootstrap_path(entry));
    KTEST_REQUIRE_TRUE(reader.next(entry) == Result::ENTRY);
    KTEST_EXPECT_ALL(!entry.directory, entry.size == 10, equal_text(entry.path, "bootstrap/echo.elf"));
    KTEST_EXPECT_TRUE(initrd::is_bootstrap_path(entry));
    KTEST_EXPECT_EQUAL(entry.data[0], static_cast<uint8_t>('e'));
    KTEST_EXPECT_EQUAL(entry.data[9], static_cast<uint8_t>('s'));
    char name[initrd::BOOTSTRAP_NAME_MAX + 1];
    KTEST_REQUIRE_TRUE(initrd::bootstrap_name(entry, name));
    KTEST_EXPECT_TRUE(equal_text(name, "echo"));
    KTEST_REQUIRE_TRUE(reader.next(entry) == Result::ENTRY);
    KTEST_EXPECT_TRUE(equal_text(entry.path, "bootstrap/elf_loader.elf"));
    KTEST_REQUIRE_TRUE(reader.next(entry) == Result::ENTRY);
    KTEST_EXPECT_TRUE(entry.directory);
    KTEST_REQUIRE_TRUE(reader.next(entry) == Result::ENTRY);
    KTEST_EXPECT_EQUAL(entry.size, 5u);
    KTEST_EXPECT_TRUE(reader.next(entry) == Result::END);
    KTEST_EXPECT_ALL(entry.data == nullptr, entry.size == 0, entry.path[0] == 0);
    KTEST_EXPECT_TRUE(reader.next(entry) == Result::END);
    reader.reset();
    KTEST_EXPECT_TRUE(reader.next(entry) == Result::ENTRY);
}

KTEST_CASE(initrd_accepts_empty_archives_and_record_padding) {
    Archive archive;
    KTEST_EXPECT_TRUE(archive.valid());
    archive.add("empty", "", 0);
    archive.size = sizeof(archive.bytes);
    KTEST_REQUIRE_TRUE(archive.valid());
    Reader reader(archive.bytes, archive.size);
    Entry entry;
    KTEST_REQUIRE_TRUE(reader.next(entry) == Result::ENTRY);
    KTEST_EXPECT_EQUAL(entry.size, 0u);
    KTEST_EXPECT_TRUE(reader.next(entry) == Result::END);
}

KTEST_CASE(initrd_accepts_prefix_and_unaligned_buffers) {
    Archive archive;
    archive.add("config", "hello", '0', "etc/archipelago");
    KTEST_REQUIRE_TRUE(archive.valid());
    uint8_t unaligned[sizeof(archive.bytes) + 1] = {};
    for (size_t i = 0; i < archive.size; i++) { unaligned[i + 1] = archive.bytes[i]; }
    Reader reader(unaligned + 1, archive.size);
    KTEST_REQUIRE_TRUE(reader.validate());
    Entry entry;
    KTEST_REQUIRE_TRUE(reader.next(entry) == Result::ENTRY);
    KTEST_EXPECT_TRUE(equal_text(entry.path, "etc/archipelago/config"));
}

KTEST_CASE(initrd_rejects_bad_magic_versions_and_checksums) {
    KTEST_EXPECT_TRUE(rejects([](uint8_t* h) { h[257] = 'x'; }));
    KTEST_EXPECT_TRUE(rejects([](uint8_t* h) { h[262] = ' '; }));
    KTEST_EXPECT_TRUE(rejects([](uint8_t* h) { h[264] = '1'; }));
    Archive archive;
    auto* header = archive.add("file", "data");
    header[0]    = 'g';  // Do not recalculate checksum for this corruption.
    KTEST_EXPECT_FALSE(archive.valid());
    checksum(header);
    header[148] = '8';
    KTEST_EXPECT_FALSE(archive.valid());
}

KTEST_CASE(initrd_rejects_extensions_links_devices_and_directory_payloads) {
    constexpr char unsupported[] = {'1', '2', '3', '4', '6', '7', 'g', 'x', 'L', 'K', 'S'};
    for (char type : unsupported) {
        KTEST_EXPECT_TRUE(rejects([type](uint8_t* h) { h[156] = static_cast<uint8_t>(type); }));
    }
    KTEST_EXPECT_TRUE(rejects([](uint8_t* h) { h[156] = '5'; }));
    KTEST_EXPECT_TRUE(rejects([](uint8_t* h) { h[157] = 'x'; }));
    KTEST_EXPECT_TRUE(rejects([](uint8_t* h) { h[500] = 'x'; }));
}

KTEST_CASE(initrd_requires_strict_octal_fields) {
    constexpr size_t fields[] = {100, 108, 116, 124, 136, 329, 337};
    for (size_t offset : fields) {
        KTEST_EXPECT_TRUE(rejects([offset](uint8_t* h) { h[offset] = '8'; }));
        KTEST_EXPECT_TRUE(rejects([offset](uint8_t* h) { h[offset] = 0x80; }));
        KTEST_EXPECT_TRUE(rejects([offset](uint8_t* h) { h[offset] = '-'; }));
        KTEST_EXPECT_TRUE(rejects([offset](uint8_t* h) {
            h[offset]     = '0';
            h[offset + 1] = 0;
            h[offset + 2] = '1';
        }));
    }
    KTEST_EXPECT_TRUE(rejects([](uint8_t* h) {
        for (size_t i = 124; i < 136; i++) { h[i] = 0; }
    }));
    KTEST_EXPECT_TRUE(rejects([](uint8_t* h) {
        for (size_t i = 124; i < 136; i++) { h[i] = '7'; }
    }));
    // Conventional leading spaces and mixed NUL/space padding remain accepted.
    Archive archive;
    auto* header = archive.add("file", "data");
    header[100]  = ' ';
    header[107]  = ' ';
    checksum(header);
    KTEST_EXPECT_TRUE(archive.valid());
}

KTEST_CASE(initrd_rejects_noncanonical_paths) {
    const char* paths[] = {"",
                           "/file",
                           "../file",
                           "a/../file",
                           "./file",
                           "a/./file",
                           "a//file",
                           "file/",
                           "a\\file",
                           "a\nfile",
                           "a\x7f"
                           "file",
                           "a\x80"
                           "file",
                           ".",
                           ".."};
    for (const char* path : paths) { KTEST_EXPECT_TRUE(rejects_path(path)); }
    KTEST_EXPECT_TRUE(rejects([](uint8_t* h) { h[99] = 'x'; }));
    KTEST_EXPECT_TRUE(rejects([](uint8_t* h) { copy_text(h + 345, "../prefix"); }));
    KTEST_EXPECT_TRUE(rejects([](uint8_t* h) { copy_text(h + 345, "prefix/"); }));
    Archive root;
    root.add("/", "", '5');
    KTEST_EXPECT_FALSE(root.valid());
    Archive double_slash;
    double_slash.add("directory//", "", '5');
    KTEST_EXPECT_FALSE(double_slash.valid());
}

KTEST_CASE(initrd_bounds_reconstructed_paths) {
    char prefix[156];
    for (size_t i = 0; i < 155; i++) { prefix[i] = 'a'; }
    prefix[155] = 0;
    char name[101];
    for (size_t i = 0; i < 100; i++) { name[i] = 'b'; }
    name[100] = 0;
    Archive oversized;
    oversized.add(name, "", '0', prefix);
    KTEST_EXPECT_FALSE(oversized.valid());
    name[99] = 0;
    Archive maximum;
    maximum.add(name, "", '0', prefix);
    KTEST_EXPECT_TRUE(maximum.valid());
    name[99] = '/';
    Archive maximum_directory;
    maximum_directory.add(name, "", '5', prefix);
    KTEST_EXPECT_TRUE(maximum_directory.valid());
    name[99] = 'b';
    Archive full_name_field;
    full_name_field.add(name);
    KTEST_EXPECT_TRUE(full_name_field.valid());
}

KTEST_CASE(initrd_rejects_duplicate_and_unsorted_paths) {
    Archive duplicate;
    duplicate.add("file", "first");
    duplicate.add("file", "second");
    KTEST_EXPECT_FALSE(duplicate.valid());
    Archive unsorted;
    unsorted.add("z", "first");
    unsorted.add("a", "second");
    KTEST_EXPECT_FALSE(unsorted.valid());
    Archive directory_alias;
    directory_alias.add("directory", "", '5');
    directory_alias.add("directory/", "", '5');
    KTEST_EXPECT_FALSE(directory_alias.valid());
    Archive prefix_alias;
    prefix_alias.add("a/b", "first");
    prefix_alias.add("b", "second", '0', "a");
    KTEST_EXPECT_FALSE(prefix_alias.valid());
}

KTEST_CASE(initrd_rejects_truncated_and_oversized_archives) {
    Archive archive;
    archive.add("file", "payload");
    KTEST_EXPECT_FALSE(Reader(nullptr, archive.size).validate());
    KTEST_EXPECT_FALSE(Reader(archive.bytes, 0).validate());
    KTEST_EXPECT_FALSE(Reader(archive.bytes, initrd::MAX_ARCHIVE_BYTES + BLOCK_SIZE).validate());
    for (size_t size = 0; size < archive.size; size++) { KTEST_REQUIRE_FALSE(Reader(archive.bytes, size).validate()); }
    write_octal(archive.bytes + 124, 12, sizeof(archive.bytes));
    checksum(archive.bytes);
    KTEST_EXPECT_FALSE(archive.valid());
}

KTEST_CASE(initrd_validates_payload_padding_and_the_complete_trailer) {
    Archive padding;
    auto* first           = padding.add("file", "data");
    first[BLOCK_SIZE + 4] = 1;
    KTEST_EXPECT_FALSE(padding.valid());
    Archive trailer;
    trailer.add("file", "data");
    trailer.bytes[trailer.size - 1] = 1;
    KTEST_EXPECT_FALSE(trailer.valid());
    // A zero block followed by another header cannot prematurely end validation.
    Archive hidden;
    hidden.add("a", "");
    hidden.used += BLOCK_SIZE;
    hidden.add("b", "");
    KTEST_EXPECT_FALSE(hidden.valid());
}

KTEST_CASE(initrd_validation_finds_late_corruption_before_iteration) {
    Archive archive;
    archive.add("bootstrap/elf_loader.elf", "loader");
    auto* second = archive.add("bootstrap/selftest.elf", "test");
    second[0]    = 'X';
    Reader reader(archive.bytes, archive.size);
    KTEST_EXPECT_FALSE(reader.validate());
    Entry entry;
    KTEST_REQUIRE_TRUE(reader.next(entry) == Result::ENTRY);
    KTEST_EXPECT_TRUE(reader.next(entry) == Result::ERROR);
    KTEST_EXPECT_ALL(entry.data == nullptr, entry.path[0] == 0, entry.size == 0);
    KTEST_EXPECT_TRUE(reader.next(entry) == Result::ERROR);
}

KTEST_CASE(initrd_bootstrap_names_match_coordinator_limits) {
    KTEST_EXPECT_TRUE(name_valid("bootstrap/elf_loader.elf"));
    KTEST_EXPECT_TRUE(name_valid("bootstrap/server_12.elf"));
    KTEST_EXPECT_TRUE(name_valid("bootstrap/abcdefghijklmnopqrstuvwxyzabcde.elf"));
    const char* invalid[] = {"bootstrap",
                             "bootstrap/",
                             "bootstrap/.elf",
                             "bootstrap/.hidden.elf",
                             "bootstrap/Elf_loader.elf",
                             "bootstrap/12server.elf",
                             "bootstrap/server-name.elf",
                             "bootstrap/nested/server.elf",
                             "bootstrap/server",
                             "bootstrap/server.ELF",
                             "bootstrap/abcdefghijklmnopqrstuvwxyzabcdef.elf",
                             "other/server.elf"};
    for (const char* path : invalid) { KTEST_EXPECT_FALSE(name_valid(path)); }
    KTEST_EXPECT_FALSE(name_valid("bootstrap/server.elf", true));
    Entry nested = {};
    copy_text(reinterpret_cast<uint8_t*>(nested.path), "bootstrap/subdir/server.elf");
    KTEST_EXPECT_TRUE(initrd::is_bootstrap_path(nested));
}
