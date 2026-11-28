#pragma once

#include <initrd/initrd.h>

namespace fileservice {

using NodeId                         = uint32_t;
constexpr NodeId ROOT                = 1;
constexpr NodeId BOOT                = 2;
constexpr size_t MAX_ARCHIVE_ENTRIES = 4096;
constexpr size_t MAX_CREATED_NODES   = 256;
constexpr size_t MAX_WRITABLE_BYTES  = 1024 * 1024;

struct Node {
    initrd::Entry entry;
    NodeId parent;
    NodeId first_child;
    NodeId last_child;
    NodeId next;
    uint16_t name_offset;
    size_t capacity;  // Allocated byte capacity for writable nodes.
};

// /boot borrows its immutable bytes from the archive mapping. The root and created
// nodes own their bytes. IDs stay valid until the entire namespace is destroyed.
class Archive {
   public:
    Archive() = default;
    ~Archive();
    Archive(const Archive&)            = delete;
    Archive& operator=(const Archive&) = delete;

    bool load(const void* bytes, size_t size);
    const Node& node(NodeId id) const { return nodes_[id]; }
    bool contains(NodeId id) const { return id != 0 && id < count_; }
    bool writable(NodeId id) const { return id == ROOT || (id >= first_created_ && contains(id)); }
    uint32_t lookup(NodeId base, const char* path, size_t length, NodeId& found) const;
    uint32_t create(NodeId parent, const char* name, size_t length, NodeId& created);
    uint32_t write(NodeId id, uint64_t offset, const char* bytes, size_t length);

   private:
    Node* nodes_           = nullptr;
    NodeId count_          = 0;
    NodeId first_created_  = 0;
    size_t writable_bytes_ = 0;
};

}  // namespace fileservice
