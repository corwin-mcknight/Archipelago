#pragma once

#include <initrd/initrd.h>

namespace fileservice {

using NodeId                         = uint32_t;
constexpr NodeId ROOT                = 1;
constexpr NodeId BOOT                = 2;
constexpr size_t MAX_ARCHIVE_ENTRIES = 4096;

struct Node {
    initrd::Entry entry;
    NodeId parent;
    NodeId first_child;
    NodeId last_child;
    NodeId next;
    uint16_t name_offset;
};

// The namespace borrows its immutable file bytes from the server's archive mapping.
class Archive {
   public:
    Archive() = default;
    ~Archive();
    Archive(const Archive&)            = delete;
    Archive& operator=(const Archive&) = delete;

    bool load(const void* bytes, size_t size);
    const Node& node(NodeId id) const { return nodes_[id]; }
    bool contains(NodeId id) const { return id != 0 && id < count_; }
    uint32_t lookup(NodeId base, const char* path, size_t length, NodeId& found) const;

   private:
    Node* nodes_  = nullptr;
    NodeId count_ = 0;
};

}  // namespace fileservice
