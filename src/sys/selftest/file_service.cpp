#include <abi/message.h>
#include <abi/syscall.h>
#include <fileservice/protocol.h>
#include <stddef.h>
#include <stdint.h>
#include <sys.h>

namespace {

using namespace fileservice;

constexpr size_t HEADER       = sizeof(abi_message_header);
constexpr size_t HANDLES_AT   = MESSAGE_BYTES;
constexpr uint64_t WAIT_NS    = 1'000'000'000;
constexpr char FIXTURE_NAME[] = "file_service.txt";
constexpr char FIXTURE_DATA[] = "Archipelago file service\n";

struct Endpoint {
    uint64_t handle = UINT64_MAX;

    void close() {
        if (handle != UINT64_MAX) { (void)sys_handle_close(handle); }
        handle = UINT64_MAX;
    }
    ~Endpoint() { close(); }
};

template <typename T> T field(size_t at) {
    T value;
    sys_copy_in(&value, at, sizeof(value));
    return value;
}

bool bytes_equal(size_t at, const char* expected, size_t length) {
    const char* data = sys_ipc_base() + at;
    for (size_t i = 0; i < length; ++i) {
        if (data[i] != expected[i]) { return false; }
    }
    return true;
}

bool send(uint64_t handle, uint32_t opcode, uint64_t txid, size_t payload = 0) {
    abi_message_header request{opcode, 0, txid};
    sys_copy_out(0, &request, HEADER);
    return sys_channel_send(handle, 0, HEADER + payload, 0, 0) == 0;
}

// Returns total reply bytes, or zero on transport/protocol failure. Payload stays in IPC storage.
size_t receive(uint64_t handle, uint32_t opcode, uint64_t txid, uint32_t status, Endpoint* endpoint = nullptr) {
    uint64_t signals = sys_object_wait(handle, ABI_CHANNEL_SIGNAL_READABLE | ABI_CHANNEL_SIGNAL_PEER_CLOSED, WAIT_NS);
    if (sys_is_error(signals) || !(signals & ABI_CHANNEL_SIGNAL_READABLE)) { return 0; }
    uint64_t result = sys_channel_recv(handle, 0, MESSAGE_BYTES, HANDLES_AT, ABI_CHANNEL_MAX_MESSAGE_HANDLES);
    if (sys_is_error(result)) { return 0; }
    size_t size = result & UINT32_MAX;
    auto header = size >= HEADER ? field<abi_message_header>(0) : abi_message_header{};
    if (size < HEADER || header.opcode != opcode || header.txid != txid || header.status != status ||
        (result >> 32) != (endpoint ? 1u : 0u)) {
        sys_close_arrived(result, HANDLES_AT);
        return 0;
    }
    if (endpoint) { endpoint->handle = field<uint64_t>(HANDLES_AT); }
    return size;
}

bool connect(Endpoint& root) {
    constexpr uint64_t txid   = 8;  // distinct from the echo discovery request
    constexpr uint64_t parent = abi::syscall::BOOTSTRAP_HANDLE;
    return send(parent, ABI_COORD_OP_CONNECT, txid, sys_stage(HEADER, NAME)) &&
           receive(parent, ABI_COORD_OP_CONNECT, txid, 0, &root) == HEADER;
}

size_t stage_open(uint32_t flags, const char* path) {
    sys_copy_out(HEADER, &flags, sizeof(flags));
    return sizeof(flags) + sys_stage(HEADER + sizeof(flags), path);
}

bool open(uint64_t directory, const char* path, Endpoint* child, uint32_t error = 0, uint32_t flags = 0) {
    constexpr uint64_t txid = 1;
    return send(directory, OPEN, txid, stage_open(flags, path)) &&
           receive(directory, OPEN, txid, error, child) == HEADER;
}

bool valid_stat(size_t size, uint32_t kind, size_t file_bytes = 0) {
    return size == HEADER + 16 && field<uint32_t>(HEADER + 8) == kind && field<uint32_t>(HEADER + 12) == 0 &&
           (kind != FILE || field<uint64_t>(HEADER) == file_bytes);
}

bool stat(uint64_t handle, uint32_t kind, size_t file_bytes = 0) {
    return send(handle, STAT, 1) && valid_stat(receive(handle, STAT, 1, 0), kind, file_bytes);
}

bool listing(uint64_t directory, const char* expected, size_t length, uint32_t kind) {
    constexpr uint64_t cursor = 0;
    sys_copy_out(HEADER, &cursor, sizeof(cursor));
    if (!send(directory, READ_DIR, 1, sizeof(cursor))) { return false; }
    const size_t size = receive(directory, READ_DIR, 1, 0);
    // Both test directories contain exactly one entry, so the first page also ends the listing.
    return size == HEADER + 12 + 3 + length && field<uint32_t>(HEADER + 8) == 1 &&
           field<uint16_t>(HEADER + 12) == length && field<uint8_t>(HEADER + 14) == kind &&
           bytes_equal(HEADER + 15, expected, length);
}

bool read(uint64_t handle, uint64_t offset, uint32_t limit, const char* expected, size_t length) {
    sys_copy_out(HEADER, &offset, sizeof(offset));
    sys_copy_out(HEADER + sizeof(offset), &limit, sizeof(limit));
    return send(handle, READ, 1, sizeof(offset) + sizeof(limit)) && receive(handle, READ, 1, 0) == HEADER + length &&
           bytes_equal(HEADER, expected, length);
}

bool print_hello(uint64_t boot) {
    Endpoint hello;
    constexpr char expected[] = "Hello, world!\n";
    constexpr uint32_t length = sizeof(expected) - 1;
    if (!open(boot, "hello.txt", &hello)) { return false; }
    sys_print("selftest: reading /boot/hello.txt\n");
    if (!read(hello.handle, 0, length + 1, expected, length)) { return false; }
    // Print the received payload directly from the IPC buffer.
    return sys_write(HEADER, length) == length;
}

bool backpressure(uint64_t directory, uint64_t other) {
    // The kernel queue has eight slots in each direction. With no reply reads, accepting
    // 17 requests proves the server has consumed at least nine, leaving a reply pending.
    constexpr uint64_t queue_depth = 8;
    constexpr uint64_t requests    = queue_depth * 2 + 1;
    for (uint64_t id = 1; id <= requests; ++id) {
        uint64_t signals =
            sys_object_wait(directory, ABI_CHANNEL_SIGNAL_WRITABLE | ABI_CHANNEL_SIGNAL_PEER_CLOSED, WAIT_NS);
        if (sys_is_error(signals) || !(signals & ABI_CHANNEL_SIGNAL_WRITABLE)) { return false; }
        if (id == queue_depth + 1) {
            // The first pending reply also owns a freshly minted file endpoint.
            if (!send(directory, OPEN, id, stage_open(0, FIXTURE_NAME))) { return false; }
        } else if (!send(directory, STAT, id)) {
            return false;
        }
    }

    if (!stat(other, FILE, sizeof(FIXTURE_DATA) - 1)) { return false; }
    Endpoint file;
    for (uint64_t id = 1; id <= requests; ++id) {
        if (id == queue_depth + 1) {
            if (receive(directory, OPEN, id, 0, &file) != HEADER) { return false; }
        } else if (!valid_stat(receive(directory, STAT, id, 0), DIRECTORY)) {
            return false;
        }
    }
    return stat(file.handle, FILE, sizeof(FIXTURE_DATA) - 1);
}

bool malformed_isolated(uint64_t directory, uint64_t other) {
    // STAT accepts an empty payload. An extra byte must close only this endpoint.
    constexpr uint8_t extra = 0;
    sys_copy_out(HEADER, &extra, sizeof(extra));
    if (!send(directory, STAT, 1, sizeof(extra))) { return false; }
    const uint64_t signals = sys_object_wait(directory, ABI_CHANNEL_SIGNAL_PEER_CLOSED, WAIT_NS);
    return !sys_is_error(signals) && (signals & ABI_CHANNEL_SIGNAL_PEER_CLOSED) &&
           static_cast<int64_t>(sys_channel_recv(directory, 0, MESSAGE_BYTES, 0, 0)) == ABI_ERR_PEER_CLOSED &&
           stat(other, FILE, sizeof(FIXTURE_DATA) - 1);
}

}  // namespace

bool file_service_selftest() {
    Endpoint root, boot, data, file;
    const size_t file_bytes = sizeof(FIXTURE_DATA) - 1;
    if (!connect(root) || !listing(root.handle, "boot", 4, DIRECTORY) || !open(root.handle, "boot", &boot) ||
        !open(boot.handle, "data", &data) || !listing(data.handle, FIXTURE_NAME, sizeof(FIXTURE_NAME) - 1, FILE) ||
        !open(data.handle, FIXTURE_NAME, &file)) {
        sys_print("selftest: file discovery or listing failed\n");
        return false;
    }

    if (!open(data.handle, "missing", nullptr, NOT_FOUND) ||
        !open(data.handle, "../bootstrap", nullptr, INVALID_PATH) ||
        !open(data.handle, "/boot", nullptr, INVALID_PATH) ||
        !open(data.handle, FIXTURE_NAME, nullptr, READ_ONLY, WRITE_ACCESS) ||
        !open(file.handle, FIXTURE_NAME, nullptr, WRONG_KIND) || !stat(file.handle, FILE, file_bytes) ||
        !read(file.handle, 0, 3, FIXTURE_DATA, 3) || !read(file.handle, 3, 100, FIXTURE_DATA + 3, file_bytes - 3) ||
        !read(file.handle, file_bytes, 1, FIXTURE_DATA, 0)) {
        sys_print("selftest: file reads or error replies failed\n");
        return false;
    }

    if (!print_hello(boot.handle)) {
        sys_print("selftest: /boot/hello.txt read or output failed\n");
        return false;
    }
    boot.close();
    root.close();
    if (!backpressure(data.handle, file.handle)) {
        sys_print("selftest: file reply backpressure failed\n");
        return false;
    }
    if (!malformed_isolated(data.handle, file.handle)) {
        sys_print("selftest: malformed file request isolation failed\n");
        return false;
    }
    return true;
}
