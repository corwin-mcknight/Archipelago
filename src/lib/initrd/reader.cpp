#include <initrd/initrd.h>

namespace initrd {
namespace {

bool zero_bytes(const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (data[i] != 0) { return false; }
    }
    return true;
}

bool matches(const uint8_t* data, const char* expected, size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (data[i] != static_cast<uint8_t>(expected[i])) { return false; }
    }
    return true;
}

// Accept conventional leading spaces and NUL/space terminators, but require actual octal digits.
// ustar leaves unused device numbers empty on regular files; only those two fields may be blank.
bool octal(const uint8_t* field, size_t size, uint64_t& value, bool blank_allowed = false) {
    value    = 0;
    size_t i = 0;
    while (i < size && field[i] == ' ') { i++; }
    const size_t first = i;
    while (i < size && field[i] >= '0' && field[i] <= '7') {
        const uint64_t digit = field[i++] - '0';
        if (value > (UINT64_MAX - digit) / 8) { return false; }
        value = value * 8 + digit;
    }
    if (i == first && !blank_allowed) { return false; }
    for (; i < size; i++) {
        if (field[i] != 0 && field[i] != ' ') { return false; }
    }
    return true;
}

bool text_length(const uint8_t* field, size_t size, size_t& length) {
    length = 0;
    while (length < size && field[length] != 0) { length++; }
    // No hidden text after a terminator, so there is exactly one interpretation of a path field.
    return zero_bytes(field + length, size - length);
}

bool canonical_path(const uint8_t* header, Entry& entry) {
    size_t name_size;
    size_t prefix_size;
    if (!text_length(header, 100, name_size) || name_size == 0 || !text_length(header + 345, 155, prefix_size)) {
        return false;
    }
    // Strip a directory's conventional final slash before bounding or copying the canonical path.
    if (entry.directory && header[name_size - 1] == '/') { name_size--; }
    const size_t size = name_size + (prefix_size != 0 ? prefix_size + 1 : 0);
    if (size > MAX_PATH_BYTES) { return false; }
    size_t length = 0;
    for (size_t i = 0; i < prefix_size; i++) { entry.path[length++] = static_cast<char>(header[345 + i]); }
    if (prefix_size != 0) { entry.path[length++] = '/'; }
    for (size_t i = 0; i < name_size; i++) { entry.path[length++] = static_cast<char>(header[i]); }
    entry.path[length] = 0;
    if (length == 0) { return false; }

    size_t component = 0;
    for (size_t i = 0; i <= length; i++) {
        const auto c = static_cast<uint8_t>(entry.path[i]);
        if (i == length || c == '/') {
            const size_t count = i - component;
            if (count == 0 || (count == 1 && entry.path[component] == '.') ||
                (count == 2 && entry.path[component] == '.' && entry.path[component + 1] == '.')) {
                return false;
            }
            component = i + 1;
        } else if (c < 0x20 || c > 0x7e || c == '\\') {
            return false;
        }
    }
    return true;
}

bool ordered_after(const char* path, const char* previous) {
    size_t i = 0;
    while (path[i] != 0 && path[i] == previous[i]) { i++; }
    return static_cast<uint8_t>(path[i]) > static_cast<uint8_t>(previous[i]);
}

bool parse_header(const uint8_t* header, Entry& entry) {
    if (!matches(header + 257, "ustar\0", 6) || !matches(header + 263, "00", 2)) { return false; }
    uint64_t expected;
    if (!octal(header + 148, 8, expected)) { return false; }
    uint64_t checksum = 0;
    for (size_t i = 0; i < BLOCK_SIZE; i++) { checksum += i >= 148 && i < 156 ? ' ' : header[i]; }
    if (checksum != expected) { return false; }

    const uint8_t type = header[156];
    if (type != '0' && type != 0 && type != '5') { return false; }
    entry.directory = type == '5';
    if (!zero_bytes(header + 157, 100) || !zero_bytes(header + 500, 12)) { return false; }

    uint64_t ignored;
    uint64_t file_size;
    if (!octal(header + 100, 8, ignored) || !octal(header + 108, 8, ignored) || !octal(header + 116, 8, ignored) ||
        !octal(header + 124, 12, file_size) || !octal(header + 136, 12, ignored) ||
        !octal(header + 329, 8, ignored, true) || !octal(header + 337, 8, ignored, true)) {
        return false;
    }
    if (file_size > MAX_ARCHIVE_BYTES || (entry.directory && file_size != 0)) { return false; }
    entry.size = static_cast<size_t>(file_size);
    return canonical_path(header, entry);
}

}  // namespace

Reader::Reader(const void* data, size_t size) : data_(static_cast<const uint8_t*>(data)), size_(size) { reset(); }

void Reader::reset() {
    offset_      = 0;
    previous_[0] = 0;
    state_       = data_ != nullptr && size_ >= 2 * BLOCK_SIZE && size_ <= MAX_ARCHIVE_BYTES && size_ % BLOCK_SIZE == 0
                       ? Result::ENTRY
                       : Result::ERROR;
}

bool Reader::validate() {
    reset();
    Entry entry;
    Result result;
    do { result = next(entry); } while (result == Result::ENTRY);
    reset();
    return result == Result::END;
}

Result Reader::next(Entry& entry) {
    entry = {};
    if (state_ != Result::ENTRY) { return state_; }
    // All advances below are bounded by size_, so subtraction cannot underflow.
    if (size_ - offset_ < BLOCK_SIZE) { return state_ = Result::ERROR; }
    const uint8_t* header = data_ + offset_;
    if (zero_bytes(header, BLOCK_SIZE)) {
        return state_ = size_ - offset_ >= 2 * BLOCK_SIZE && zero_bytes(header, size_ - offset_) ? Result::END
                                                                                                 : Result::ERROR;
    }
    Entry candidate = {};
    if (!parse_header(header, candidate) || !ordered_after(candidate.path, previous_)) {
        return state_ = Result::ERROR;
    }
    const size_t body_offset = offset_ + BLOCK_SIZE;
    // file size was capped before rounding, and all remaining bounds use subtraction.
    const size_t body_size   = (candidate.size + BLOCK_SIZE - 1) / BLOCK_SIZE * BLOCK_SIZE;
    if (body_size > size_ - body_offset ||
        !zero_bytes(data_ + body_offset + candidate.size, body_size - candidate.size)) {
        return state_ = Result::ERROR;
    }
    candidate.data = data_ + body_offset;
    offset_        = body_offset + body_size;
    for (size_t i = 0; i <= MAX_PATH_BYTES; i++) { previous_[i] = candidate.path[i]; }
    entry = candidate;
    return Result::ENTRY;
}

bool is_bootstrap_path(const Entry& entry) {
    constexpr char PREFIX[] = "bootstrap/";
    for (size_t i = 0; i < sizeof(PREFIX) - 1; i++) {
        if (entry.path[i] != PREFIX[i]) { return false; }
    }
    return true;
}

bool bootstrap_name(const Entry& entry, char out[BOOTSTRAP_NAME_MAX + 1]) {
    out[0] = 0;
    if (entry.directory || !is_bootstrap_path(entry)) { return false; }
    constexpr size_t PREFIX_SIZE = sizeof("bootstrap/") - 1;
    const char* name             = entry.path + PREFIX_SIZE;
    size_t size                  = 0;
    while (PREFIX_SIZE + size <= MAX_PATH_BYTES && name[size] != 0) { size++; }
    constexpr size_t SUFFIX_SIZE = sizeof(".elf") - 1;
    if (size <= SUFFIX_SIZE || size > BOOTSTRAP_NAME_MAX + SUFFIX_SIZE) { return false; }
    const size_t name_size = size - SUFFIX_SIZE;
    if (!matches(reinterpret_cast<const uint8_t*>(name + name_size), ".elf", SUFFIX_SIZE)) { return false; }
    for (size_t i = 0; i < name_size; i++) {
        const char c = name[i];
        if (!(c >= 'a' && c <= 'z') && (i == 0 || !((c >= '0' && c <= '9') || c == '_'))) { return false; }
    }
    for (size_t i = 0; i < name_size; i++) { out[i] = name[i]; }
    out[name_size] = 0;
    return true;
}

}  // namespace initrd
