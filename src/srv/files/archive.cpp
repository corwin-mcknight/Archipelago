#include "archive.h"

#include <fileservice/protocol.h>

extern "C" void* malloc(size_t);
extern "C" void free(void*);

namespace fileservice {
namespace {

size_t length_of(const char* text) {
    size_t length = 0;
    while (text[length] != '\0') { ++length; }
    return length;
}

int compare(const char* left, size_t length, const char* right) {
    for (size_t i = 0; i < length; ++i) {
        if (right[i] == '\0') { return 1; }
        if (left[i] != right[i]) { return static_cast<unsigned char>(left[i]) - static_cast<unsigned char>(right[i]); }
    }
    return right[length] == '\0' ? 0 : -1;
}

bool valid_component(const char* name, size_t size) {
    if (size == 0 || (size == 1 && name[0] == '.') || (size == 2 && name[0] == '.' && name[1] == '.')) { return false; }
    for (size_t i = 0; i < size; ++i) {
        if (name[i] < ' ' || name[i] > '~' || name[i] == '\\' || name[i] == '/') { return false; }
    }
    return true;
}

}  // namespace

Archive::~Archive() {
    for (NodeId id = ROOT; id < count_; ++id) {
        if (writable(id)) { free(const_cast<uint8_t*>(nodes_[id].entry.data)); }
    }
    free(nodes_);
}

bool Archive::load(const void* bytes, size_t size) {
    if (nodes_ != nullptr) { return false; }
    initrd::Reader reader(bytes, size);
    initrd::Entry entry;
    NodeId count = BOOT + 1;
    for (;;) {
        auto result = reader.next(entry);
        if (result == initrd::Result::END) { break; }
        if (result == initrd::Result::ERROR || count == MAX_ARCHIVE_ENTRIES + BOOT + 1) { return false; }
        ++count;
    }
    auto* nodes = static_cast<Node*>(malloc((count + MAX_CREATED_NODES) * sizeof(Node)));
    if (nodes == nullptr) { return false; }

    nodes[ROOT]                 = {};
    nodes[ROOT].entry.directory = true;
    nodes[ROOT].first_child = nodes[ROOT].last_child = BOOT;
    nodes[BOOT]                                      = {};
    nodes[BOOT].entry.directory                      = true;
    nodes[BOOT].entry.path[0]                        = 'b';
    nodes[BOOT].entry.path[1]                        = 'o';
    nodes[BOOT].entry.path[2]                        = 'o';
    nodes[BOOT].entry.path[3]                        = 't';
    nodes[BOOT].parent                               = ROOT;

    reader.reset();
    for (NodeId id = BOOT + 1; id < count; ++id) {
        if (reader.next(entry) != initrd::Result::ENTRY) {
            free(nodes);
            return false;
        }
        const size_t length = length_of(entry.path);
        size_t name_offset  = length;
        while (name_offset != 0 && entry.path[name_offset - 1] != '/') { --name_offset; }
        NodeId parent = BOOT;
        if (name_offset != 0) {
            // A canonical parent sorts before its descendants. Search all preceding records.
            NodeId lo = BOOT + 1;
            NodeId hi = id;
            while (lo < hi) {
                const NodeId mid = lo + (hi - lo) / 2;
                if (compare(entry.path, name_offset - 1, nodes[mid].entry.path) > 0) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            if (lo == id || compare(entry.path, name_offset - 1, nodes[lo].entry.path) != 0 ||
                !nodes[lo].entry.directory) {
                free(nodes);
                return false;
            }
            parent = lo;
        }
        Node& node = nodes[id];
        __builtin_memcpy(&node.entry, &entry, sizeof(entry));
        node.parent      = parent;
        node.first_child = node.last_child = node.next = 0;
        node.name_offset                               = static_cast<uint16_t>(name_offset);
        node.capacity                                  = 0;
        auto& directory                                = nodes[parent];
        if (directory.last_child == 0) {
            directory.first_child = id;
        } else {
            nodes[directory.last_child].next = id;
        }
        directory.last_child = id;
    }
    nodes_         = nodes;
    count_         = count;
    first_created_ = count;
    return true;
}

uint32_t Archive::lookup(NodeId base, const char* path, size_t length, NodeId& found) const {
    if (length == 0 || length > initrd::MAX_PATH_BYTES) { return INVALID_PATH; }
    // Validate the entire path before traversal, including suffixes after a missing component.
    for (size_t start = 0, i = 0; i <= length; ++i) {
        if (i != length && path[i] != '/') { continue; }
        if (!valid_component(path + start, i - start)) { return INVALID_PATH; }
        start = i + 1;
    }

    NodeId current = base;
    for (size_t start = 0; start < length;) {
        if (!writable(current) && !nodes_[current].entry.directory) { return WRONG_KIND; }
        size_t end = start;
        while (end < length && path[end] != '/') { ++end; }
        NodeId child = nodes_[current].first_child;
        for (; child != 0; child = nodes_[child].next) {
            const auto& node = nodes_[child];
            const int order  = compare(path + start, end - start, node.entry.path + node.name_offset);
            if (order == 0) { break; }
            if (order < 0) { return NOT_FOUND; }
        }
        if (child == 0) { return NOT_FOUND; }
        current = child;
        start   = end + 1;
    }
    found = current;
    return 0;
}

uint32_t Archive::create(NodeId parent_id, const char* name, size_t length, NodeId& created) {
    if (!writable(parent_id)) { return READ_ONLY; }
    Node& parent        = nodes_[parent_id];
    const size_t prefix = parent_id == ROOT ? 0 : length_of(parent.entry.path) + 1;
    if (prefix > initrd::MAX_PATH_BYTES || length > initrd::MAX_PATH_BYTES - prefix || !valid_component(name, length)) {
        return INVALID_PATH;
    }

    // The cursor in a directory page names the next child. Inserting a sibling
    // only changes a link; existing child IDs continue to identify the same nodes.
    NodeId* link = &parent.first_child;
    while (*link != 0) {
        Node& sibling   = nodes_[*link];
        const int order = compare(name, length, sibling.entry.path + sibling.name_offset);
        if (order == 0) { return ALREADY_EXISTS; }
        if (order < 0) { break; }
        link = &sibling.next;
    }
    if (count_ - first_created_ == MAX_CREATED_NODES) { return RESOURCE_EXHAUSTED; }

    const NodeId id = count_++;
    Node& child     = nodes_[id];
    child           = {};
    if (prefix != 0) {
        __builtin_memcpy(child.entry.path, parent.entry.path, prefix - 1);
        child.entry.path[prefix - 1] = '/';
    }
    __builtin_memcpy(child.entry.path + prefix, name, length);
    child.name_offset = static_cast<uint16_t>(prefix);
    child.parent      = parent_id;
    child.next        = *link;
    *link             = id;
    if (child.next == 0) { parent.last_child = id; }
    parent.entry.directory = true;
    created                = id;
    return 0;
}

uint32_t Archive::write(NodeId id, uint64_t offset, const char* bytes, size_t length) {
    if (!writable(id)) { return READ_ONLY; }
    Node& node  = nodes_[id];
    auto& entry = node.entry;
    if (offset > entry.size) { return INVALID_OFFSET; }
    if (length == 0) { return 0; }
    const size_t start = static_cast<size_t>(offset);
    // Check the available budget before adding an untrusted length.
    if (length > (entry.size - start) + (MAX_WRITABLE_BYTES - writable_bytes_)) { return RESOURCE_EXHAUSTED; }
    const size_t end = start + length;
    auto* data       = const_cast<uint8_t*>(entry.data);
    if (end > node.capacity) {
        size_t capacity = node.capacity == 0 ? 1 : node.capacity;
        while (capacity < end) { capacity *= 2; }
        auto* replacement = static_cast<uint8_t*>(malloc(capacity));
        if (replacement == nullptr) { return RESOURCE_EXHAUSTED; }
        if (entry.size != 0) { __builtin_memcpy(replacement, data, entry.size); }
        free(data);
        data          = replacement;
        entry.data    = replacement;
        node.capacity = capacity;
    }
    __builtin_memcpy(data + start, bytes, length);
    if (end > entry.size) {
        writable_bytes_ += end - entry.size;
        entry.size = end;
    }
    return 0;
}

}  // namespace fileservice
