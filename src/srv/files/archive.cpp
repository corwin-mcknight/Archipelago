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
        if (name[i] < ' ' || name[i] > '~' || name[i] == '\\') { return false; }
    }
    return true;
}

}  // namespace

Archive::~Archive() { free(nodes_); }

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
    auto* nodes = static_cast<Node*>(malloc(count * sizeof(Node)));
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
        auto& directory                                = nodes[parent];
        if (directory.last_child == 0) {
            directory.first_child = id;
        } else {
            nodes[directory.last_child].next = id;
        }
        directory.last_child = id;
    }
    nodes_ = nodes;
    count_ = count;
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
        if (!nodes_[current].entry.directory) { return WRONG_KIND; }
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

}  // namespace fileservice
