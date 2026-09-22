#pragma once

#include <stddef.h>
#include <stdint.h>

// Allocation-free userspace reader for Archipelago's uncompressed POSIX ustar initrd profile.
// Regular files and empty directory records only; no links, extensions, or compression. Paths are
// printable ASCII, relative, at most 255 bytes, with no backslashes, empty, '.' or '..' components.
// A directory's optional final slash is removed. Entries must be strictly lexicographically ordered
// by these canonical paths, which also rejects duplicates without allocation. Numeric fields use
// octal, payload padding is zero, and at least two zero blocks terminate the archive.
//
// The buffer is borrowed and must stay immutable while it is read. Call validate() before acting on
// any entries: next() validates incrementally and cannot vouch for entries it has not reached yet.
namespace initrd {

constexpr size_t BLOCK_SIZE         = 512;
constexpr size_t MAX_ARCHIVE_BYTES  = 64 * 1024 * 1024;
constexpr size_t MAX_PATH_BYTES     = 255;
constexpr size_t BOOTSTRAP_NAME_MAX = 31;

struct Entry {
    const uint8_t* data;
    size_t size;
    char path[MAX_PATH_BYTES + 1];
    bool directory;
};

enum class Result { ENTRY, END, ERROR };

class Reader {
   public:
    Reader(const void* data, size_t size);
    // Validates the complete archive and rewinds, whether validation succeeds or fails.
    bool validate();
    void reset();
    // ERROR and END remain sticky until reset(). Entry is cleared unless ENTRY is returned.
    Result next(Entry& entry);

   private:
    const uint8_t* data_;
    size_t size_;
    size_t offset_;
    char previous_[MAX_PATH_BYTES + 1];
    Result state_;
};

// All descendants of bootstrap/, including nested directories; the root 'bootstrap' is excluded.
bool is_bootstrap_path(const Entry& entry);
// Regular files named bootstrap/[a-z][a-z0-9_]*.elf; writes the service name and its terminating NUL.
bool bootstrap_name(const Entry& entry, char out[BOOTSTRAP_NAME_MAX + 1]);

}  // namespace initrd
