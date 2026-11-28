#pragma once

#include <abi/message.h>
#include <abi/syscall.h>
#include <fileservice/protocol.h>
#include <stddef.h>
#include <stdint.h>
#include <sys.h>

namespace file_test {

using namespace fileservice;

constexpr size_t HEADER      = sizeof(abi_message_header);
constexpr size_t HANDLES_AT  = MESSAGE_BYTES;
constexpr uint64_t WAIT_NS   = 1'000'000'000;
constexpr char WRITER_DONE[] = "file-demo-done";
constexpr char HELLO[]       = "Hello, world!\n";

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

inline bool bytes_equal(size_t at, const char* expected, size_t length) {
    const char* data = sys_ipc_base() + at;
    for (size_t i = 0; i < length; ++i) {
        if (data[i] != expected[i]) { return false; }
    }
    return true;
}

inline bool send(uint64_t handle, uint32_t opcode, uint64_t txid, size_t payload = 0) {
    abi_message_header request{opcode, 0, txid};
    sys_copy_out(0, &request, HEADER);
    return sys_channel_send(handle, 0, HEADER + payload, 0, 0) == 0;
}

// Returns total reply bytes, or zero on transport/protocol failure. Payload stays in IPC storage.
inline size_t receive(uint64_t handle, uint32_t opcode, uint64_t txid, uint32_t status, Endpoint* endpoint = nullptr,
                      uint64_t timeout = WAIT_NS) {
    uint64_t signals = sys_object_wait(handle, ABI_CHANNEL_SIGNAL_READABLE | ABI_CHANNEL_SIGNAL_PEER_CLOSED, timeout);
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

inline bool connect(Endpoint& root, const char* name = NAME) {
    constexpr uint64_t txid   = 8;  // distinct from the echo discovery request
    constexpr uint64_t parent = abi::syscall::BOOTSTRAP_HANDLE;
    return send(parent, ABI_COORD_OP_CONNECT, txid, sys_stage(HEADER, name)) &&
           receive(parent, ABI_COORD_OP_CONNECT, txid, 0, &root, STARTUP_TIMEOUT_NS) == HEADER;
}

inline size_t stage_open(uint32_t flags, const char* path) {
    sys_copy_out(HEADER, &flags, sizeof(flags));
    return sizeof(flags) + sys_stage(HEADER + sizeof(flags), path);
}

inline bool open(uint64_t directory, const char* path, Endpoint* child, uint32_t error = 0, uint32_t flags = 0) {
    constexpr uint64_t txid = 1;
    return send(directory, OPEN, txid, stage_open(flags, path)) &&
           receive(directory, OPEN, txid, error, child) == HEADER;
}

inline bool listing(uint64_t directory, const char* expected, size_t length, uint32_t kind) {
    constexpr uint64_t cursor = 0;
    sys_copy_out(HEADER, &cursor, sizeof(cursor));
    if (!send(directory, READ_DIR, 1, sizeof(cursor))) { return false; }
    const size_t size = receive(directory, READ_DIR, 1, 0);
    // This fixture expects a single child and a complete first page.
    return size == HEADER + 12 + 3 + length && field<uint32_t>(HEADER + 8) == 1 && field<uint64_t>(HEADER) == 0 &&
           field<uint16_t>(HEADER + 12) == length && field<uint8_t>(HEADER + 14) == kind &&
           bytes_equal(HEADER + 15, expected, length);
}

inline bool request_node(uint64_t parent, uint32_t opcode, const char* name, uint32_t rights, uint32_t ceiling,
                         Endpoint* child, uint32_t status = 0) {
    sys_copy_out(HEADER, &rights, sizeof(rights));
    sys_copy_out(HEADER + sizeof(rights), &ceiling, sizeof(ceiling));
    constexpr size_t fixed = sizeof(rights) + sizeof(ceiling);
    size_t length          = sys_stage(HEADER + fixed, name);
    return send(parent, opcode, 1, fixed + length) && receive(parent, opcode, 1, status, child) == HEADER;
}

inline bool write(uint64_t handle, uint64_t offset, const char* bytes, uint32_t status = 0) {
    sys_copy_out(HEADER, &offset, sizeof(offset));
    size_t length = sys_stage(HEADER + sizeof(offset), bytes);
    return send(handle, WRITE, 1, sizeof(offset) + length) && receive(handle, WRITE, 1, status) == HEADER;
}

}  // namespace file_test
