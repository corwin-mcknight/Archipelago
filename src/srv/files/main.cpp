#include <abi/message.h>
#include <abi/syscall.h>
#include <fileservice/protocol.h>
#include <sys.h>

#include "access.h"
#include "archive.h"

namespace {
using namespace fileservice;

constexpr size_t ENDPOINT_LIMIT  = 64;
constexpr uint64_t NONE          = UINT64_MAX;
constexpr uint64_t PARENT        = ABI_BOOTSTRAP_HANDLE;
constexpr uint64_t PARENT_KEY    = ENDPOINT_LIMIT;
constexpr uint64_t REGISTER_TXID = 2;
constexpr uint64_t CLOSED        = ABI_CHANNEL_SIGNAL_PEER_CLOSED;
constexpr uint64_t READABLE      = ABI_CHANNEL_SIGNAL_READABLE;
constexpr uint64_t WRITABLE      = ABI_CHANNEL_SIGNAL_WRITABLE;
constexpr size_t RECEIVE_AT      = MESSAGE_BYTES;
constexpr size_t HANDLES_AT      = 0;  // Parent receive only
constexpr size_t SEND_HANDLE_AT  = RECEIVE_AT;
constexpr size_t DATA_AT         = sizeof(abi_message_header);

struct Reply {
    char bytes[MESSAGE_BYTES];
    size_t size;
    uint64_t handle;
};
struct Endpoint {
    uint64_t handle;
    NodeId node;  // Zero means unused
    Access access;
    Reply reply;
};

[[clang::no_destroy]] Archive archive;
Endpoint endpoints[ENDPOINT_LIMIT];
uint64_t port;

void copy_bytes(char* out, const void* source, size_t size) {
    const auto* in = static_cast<const char*>(source);
    for (size_t i = 0; i < size; ++i) { out[i] = in[i]; }
}

template <typename T> void put(Reply& reply, T value) {
    copy_bytes(reply.bytes + reply.size, &value, sizeof(value));
    reply.size += sizeof(value);
}

template <typename T> T get(size_t offset) {
    T value;
    sys_copy_in(&value, RECEIVE_AT + DATA_AT + offset, sizeof(value));
    return value;
}

void close_endpoint(size_t slot) {
    auto& endpoint = endpoints[slot];
    (void)sys_port_unbind(port, slot);
    if (endpoint.reply.handle != NONE) { (void)sys_handle_close(endpoint.reply.handle); }
    (void)sys_handle_close(endpoint.handle);
    endpoint.node = 0;
}

size_t attach(uint64_t handle, NodeId node, Access access) {
    for (size_t i = 0; i < ENDPOINT_LIMIT; ++i) {
        if (endpoints[i].node != 0) { continue; }
        if (sys_is_error(sys_port_bind(port, handle, i, READABLE | CLOSED))) { break; }
        endpoints[i].handle       = handle;
        endpoints[i].node         = node;
        endpoints[i].access       = access;
        endpoints[i].reply.size   = 0;
        endpoints[i].reply.handle = NONE;
        return i;
    }
    return ENDPOINT_LIMIT;
}

uint32_t open_node(const Endpoint& endpoint, uint32_t opcode, size_t length, Reply& reply) {
    const bool create      = opcode == CREATE;
    const bool legacy      = opcode == OPEN;
    const size_t prefix    = legacy ? sizeof(uint32_t) : 2 * sizeof(uint32_t);
    const char* path       = sys_ipc_base() + RECEIVE_AT + DATA_AT + prefix;
    const size_t path_size = length - prefix;
    if (create && !archive.writable(endpoint.node)) { return READ_ONLY; }
    if (!endpoint.access.allows(create ? CREATE_CHILD : LOOKUP)) { return ACCESS_DENIED; }

    constexpr uint32_t browse = READ_BYTES | LIST | LOOKUP;
    Access child{};
    const uint32_t first = get<uint32_t>(0);
    if (legacy) {
        if (first & ~WRITE_ACCESS) { return UNSUPPORTED; }
        child.rights = READ_BYTES | (browse & endpoint.access.ceiling);
        if (first & WRITE_ACCESS) { child.rights |= READ_WRITE; }
        child.ceiling = child.rights;
    } else {
        child = {first, get<uint32_t>(sizeof(uint32_t))};
    }

    NodeId found = endpoint.node;
    if (!create) {
        // Every intermediate component needs lookup authority as well.
        if (!(endpoint.access.ceiling & LOOKUP)) {
            for (size_t i = 0; i < path_size; ++i) {
                if (path[i] == '/') { return ACCESS_DENIED; }
            }
        }
        if (const auto status = archive.lookup(endpoint.node, path, path_size, found)) { return status; }
        constexpr uint32_t mutation = APPEND_BYTES | OVERWRITE_BYTES | CREATE_CHILD;
        if (!archive.writable(found) && ((child.rights | child.ceiling) & mutation)) { return READ_ONLY; }
    }
    if (const auto status = endpoint.access.grant(child)) { return status; }

    if (sys_is_error(sys_channel_create(HANDLES_AT))) { return RESOURCE_EXHAUSTED; }
    uint64_t handles[2];
    sys_copy_in(handles, HANDLES_AT, sizeof(handles));
    const size_t slot = attach(handles[0], found, child);
    if (slot == ENDPOINT_LIMIT) {
        (void)sys_handle_close(handles[0]);
        (void)sys_handle_close(handles[1]);
        return RESOURCE_EXHAUSTED;
    }
    if (create) {
        // Reserve the complete reply channel before publishing the new name.
        if (const auto status = archive.create(endpoint.node, path, path_size, found)) {
            close_endpoint(slot);
            (void)sys_handle_close(handles[1]);
            return status;
        }
        endpoints[slot].node = found;
    }
    reply.handle = handles[1];
    return 0;
}

uint32_t read_file(const Endpoint& endpoint, Reply& reply) {
    if (!endpoint.access.allows(READ_BYTES)) { return ACCESS_DENIED; }
    const auto& node = archive.node(endpoint.node);
    if (!archive.writable(endpoint.node) && node.entry.directory) { return WRONG_KIND; }
    const auto offset    = get<uint64_t>(0);
    const auto requested = get<uint32_t>(sizeof(offset));
    if (offset >= node.entry.size) { return 0; }
    size_t size = node.entry.size - offset;
    if (size > requested) { size = requested; }
    if (size > MESSAGE_BYTES - DATA_AT) { size = MESSAGE_BYTES - DATA_AT; }
    copy_bytes(reply.bytes + reply.size, node.entry.data + offset, size);
    reply.size += size;
    return 0;
}

uint32_t write_node(const Endpoint& endpoint, size_t length) {
    if (!archive.writable(endpoint.node)) { return READ_ONLY; }
    uint64_t offset   = get<uint64_t>(0);
    const bool append = offset == APPEND_OFFSET;
    if (!endpoint.access.allows(append ? APPEND_BYTES : OVERWRITE_BYTES)) { return ACCESS_DENIED; }
    if (append) { offset = archive.node(endpoint.node).entry.size; }
    const char* bytes = sys_ipc_base() + RECEIVE_AT + DATA_AT + sizeof(offset);
    return archive.write(endpoint.node, offset, bytes, length - sizeof(offset));
}

uint32_t list_directory(const Endpoint& endpoint, Reply& reply) {
    if (!endpoint.access.allows(LIST)) { return ACCESS_DENIED; }
    const NodeId id       = endpoint.node;
    const auto& directory = archive.node(id);
    if (!archive.writable(id) && !directory.entry.directory) { return WRONG_KIND; }
    const auto cursor = get<uint64_t>(0);
    if (cursor != 0 && (cursor > UINT32_MAX || !archive.contains(static_cast<NodeId>(cursor)) ||
                        archive.node(static_cast<NodeId>(cursor)).parent != id)) {
        return INVALID_CURSOR;
    }
    NodeId next = cursor == 0 ? directory.first_child : static_cast<NodeId>(cursor);
    reply.size += sizeof(uint64_t) + sizeof(uint32_t);
    while (next != 0) {
        const auto& child = archive.node(next);
        const char* name  = child.entry.path + child.name_offset;
        uint16_t length   = 0;
        while (name[length] != '\0') { ++length; }
        if (reply.size + sizeof(length) + sizeof(uint8_t) + length > MESSAGE_BYTES) { break; }
        put(reply, length);
        put(reply, static_cast<uint8_t>(child.entry.directory ? DIRECTORY : FILE));
        copy_bytes(reply.bytes + reply.size, name, length);
        reply.size += length;
        next = child.next;
    }
    const uint64_t continuation = next;
    const uint32_t end          = next == 0;
    copy_bytes(reply.bytes + DATA_AT, &continuation, sizeof(continuation));
    copy_bytes(reply.bytes + DATA_AT + sizeof(continuation), &end, sizeof(end));
    return 0;
}

bool prepare_reply(Endpoint& endpoint, size_t size) {
    if (size < DATA_AT) { return false; }
    abi_message_header header;
    sys_copy_in(&header, RECEIVE_AT, sizeof(header));
    if (header.status != 0 || header.txid == 0) { return false; }

    const size_t payload = size - DATA_AT;
    const Node& node     = archive.node(endpoint.node);
    auto& reply          = endpoint.reply;
    reply.size           = DATA_AT;
    switch (header.opcode) {
        case OPEN:
            if (payload < sizeof(uint32_t)) { return false; }
            header.status = open_node(endpoint, OPEN, payload, reply);
            break;
        case OPEN_RIGHTS:
        case CREATE:
            if (payload < 2 * sizeof(uint32_t)) { return false; }
            header.status = open_node(endpoint, header.opcode, payload, reply);
            break;
        case WRITE:
            if (payload < sizeof(uint64_t)) { return false; }
            header.status = write_node(endpoint, payload);
            break;
        case STAT:
            if (payload != 0) { return false; }
            put(reply, static_cast<uint64_t>(node.entry.size));
            put(reply, node.entry.directory ? DIRECTORY : FILE);
            put(reply, uint32_t{0});
            break;
        case READ:
            if (payload != sizeof(uint64_t) + sizeof(uint32_t)) { return false; }
            header.status = read_file(endpoint, reply);
            break;
        case READ_DIR:
            if (payload != sizeof(uint64_t)) { return false; }
            header.status = list_directory(endpoint, reply);
            break;
        default: header.status = UNSUPPORTED; break;
    }
    copy_bytes(reply.bytes, &header, DATA_AT);
    return true;
}

bool send_reply(Endpoint& endpoint) {
    const uint64_t signals = sys_object_wait(endpoint.handle, 0, 0);
    if (sys_is_error(signals) || (signals & CLOSED)) { return false; }
    if (!(signals & WRITABLE)) { return true; }

    // This single-threaded service is the only sender on this end. After WRITABLE the peer
    // can only drain the queue or close it. An attempted send failure is terminal: attached
    // handles may already have been consumed, so we never retry them.
    auto& reply = endpoint.reply;
    sys_copy_out(0, reply.bytes, reply.size);
    const bool attached = reply.handle != NONE;
    if (attached) { sys_copy_out(SEND_HANDLE_AT, &reply.handle, sizeof(reply.handle)); }
    if (sys_is_error(sys_channel_send(endpoint.handle, 0, reply.size, SEND_HANDLE_AT, attached ? 1 : 0))) {
        return false;
    }
    reply.handle = NONE;
    reply.size   = 0;
    return true;
}

void serve_endpoint(size_t slot) {
    auto& endpoint         = endpoints[slot];
    const uint64_t signals = sys_object_wait(endpoint.handle, 0, 0);
    if (sys_is_error(signals) || (signals & CLOSED)) {
        close_endpoint(slot);
        return;
    }
    if (endpoint.reply.size == 0 && (signals & READABLE)) {
        // No request takes handles. A zero handle capacity makes the kernel drop any
        // attachments and report TRUNCATED, even if it could not install them for us.
        const uint64_t got = sys_channel_recv(endpoint.handle, RECEIVE_AT, MESSAGE_BYTES, 0, 0);
        if (sys_is_error(got) || !prepare_reply(endpoint, got)) {
            close_endpoint(slot);
            return;
        }
    }
    if (endpoint.reply.size != 0 && !send_reply(endpoint)) {
        close_endpoint(slot);
        return;
    }

    // Rebind to sample current readiness and yield after one request. This both preserves
    // fairness and notices requests that arrived while READABLE was masked under backpressure.
    (void)sys_port_unbind(port, slot);
    const uint64_t mask = CLOSED | (endpoint.reply.size != 0 ? WRITABLE : READABLE);
    if (sys_is_error(sys_port_bind(port, endpoint.handle, slot, mask))) { close_endpoint(slot); }
}

bool name_matches(size_t size) {
    if (size != sizeof(NAME) - 1) { return false; }
    const char* name = sys_ipc_base() + RECEIVE_AT + DATA_AT;
    for (size_t i = 0; i < size; ++i) {
        if (name[i] != NAME[i]) { return false; }
    }
    return true;
}

bool serve_parent() {
    for (;;) {
        const uint64_t got = sys_channel_recv(PARENT, RECEIVE_AT, MESSAGE_BYTES, HANDLES_AT, 4);
        if (got == static_cast<uint64_t>(ABI_ERR_WOULD_BLOCK)) { return true; }
        if (sys_is_error(got)) { return false; }
        abi_message_header header{};
        const size_t size    = static_cast<uint32_t>(got);
        const uint64_t count = got >> 32;
        if (size >= DATA_AT) { sys_copy_in(&header, RECEIVE_AT, DATA_AT); }

        if (header.opcode == ABI_COORD_OP_REGISTER && header.txid == REGISTER_TXID && size == DATA_AT && count == 0 &&
            header.status == 0) {
            continue;
        }
        if (header.opcode != ABI_COORD_OP_CONNECTION || header.status != 0 || header.txid != 0 || count != 1 ||
            size < DATA_AT || !name_matches(size - DATA_AT)) {
            sys_close_arrived(got, HANDLES_AT);
            return false;
        }
        uint64_t handle;
        sys_copy_in(&handle, HANDLES_AT, sizeof(handle));
        if (attach(handle, ROOT, {ALL_RIGHTS, ALL_RIGHTS}) == ENDPOINT_LIMIT) { (void)sys_handle_close(handle); }
    }
}

bool receive_archive() {
    const uint64_t signals = sys_object_wait(PARENT, READABLE | CLOSED, STARTUP_TIMEOUT_NS);
    if (sys_is_error(signals) || (signals & CLOSED)) { return false; }
    const uint64_t got = sys_channel_recv(PARENT, RECEIVE_AT, MESSAGE_BYTES, HANDLES_AT, 4);
    if (sys_is_error(got)) { return false; }
    if ((got >> 32) != 1 || static_cast<uint32_t>(got) != DATA_AT + sizeof(abi_image_payload)) {
        sys_close_arrived(got, HANDLES_AT);
        return false;
    }
    abi_message_header header;
    sys_copy_in(&header, RECEIVE_AT, DATA_AT);
    const auto payload = get<abi_image_payload>(0);
    uint64_t vmo;
    sys_copy_in(&vmo, HANDLES_AT, sizeof(vmo));
    uint64_t mapping = NONE;
    if (header.opcode == ARCHIVE && header.status == 0 && header.txid == ARCHIVE_TXID && payload.size_bytes != 0 &&
        payload.size_bytes <= initrd::MAX_ARCHIVE_BYTES) {
        const uint64_t rounded = (payload.size_bytes + ABI_VM_PAGE_SIZE - 1) & ~(ABI_VM_PAGE_SIZE - 1);
        mapping                = sys_vmo_map(vmo, 0, 0, rounded, ABI_VM_PROT_READ);
    }
    // A successful mapping retains its own reference for the lifetime of the service.
    (void)sys_handle_close(vmo);
    return !sys_is_error(mapping) && archive.load(reinterpret_cast<const void*>(mapping), payload.size_bytes);
}

bool startup() {
    const uint64_t got = sys_channel_recv(PARENT, RECEIVE_AT, MESSAGE_BYTES, HANDLES_AT, 4);
    if (sys_is_error(got)) { return false; }
    sys_close_arrived(got, HANDLES_AT);
    if ((got >> 32) != 3 || !receive_archive()) {
        sys_print("files: cannot receive or index initrd\n");
        return false;
    }
    port = sys_port_create();
    if (sys_is_error(port) || sys_is_error(sys_port_bind(port, PARENT, PARENT_KEY, READABLE | CLOSED))) {
        sys_print("files: cannot bind parent mailbox\n");
        return false;
    }
    // Init treats this registration on the designated child's mailbox as readiness.
    const abi_message_header registration{ABI_COORD_OP_REGISTER, 0, REGISTER_TXID};
    sys_copy_out(0, &registration, DATA_AT);
    sys_copy_out(DATA_AT, NAME, sizeof(NAME) - 1);
    return !sys_is_error(sys_channel_send(PARENT, 0, DATA_AT + sizeof(NAME) - 1, 0, 0));
}

}  // namespace

extern "C" int main() {
    if (!startup()) { return 1; }
    for (;;) {
        if (sys_is_error(sys_port_wait(port, 0, 0))) { return 1; }
        uint64_t key;
        sys_copy_in(&key, 0, sizeof(key));
        if (key == PARENT_KEY) {
            if (!serve_parent()) { return 0; }
        } else if (key < ENDPOINT_LIMIT && endpoints[key].node != 0) {
            serve_endpoint(key);
        }
    }
}
