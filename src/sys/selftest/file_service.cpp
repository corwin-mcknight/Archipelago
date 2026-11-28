#include "file_client.h"

namespace {
using namespace file_test;

constexpr char FIXTURE_NAME[] = "file_service.txt";
constexpr char FIXTURE_DATA[] = "Archipelago file service\n";

bool valid_stat(size_t size, uint32_t kind, size_t file_bytes = 0) {
    return size == HEADER + 16 && field<uint32_t>(HEADER + 8) == kind && field<uint32_t>(HEADER + 12) == 0 &&
           field<uint64_t>(HEADER) == file_bytes;
}

bool stat(uint64_t handle, uint32_t kind, size_t file_bytes = 0) {
    return send(handle, STAT, 1) && valid_stat(receive(handle, STAT, 1, 0), kind, file_bytes);
}

bool read(uint64_t handle, uint64_t offset, uint32_t limit, const char* expected, size_t length, uint32_t status = 0) {
    sys_copy_out(HEADER, &offset, sizeof(offset));
    sys_copy_out(HEADER + sizeof(offset), &limit, sizeof(limit));
    return send(handle, READ, 1, sizeof(offset) + sizeof(limit)) &&
           receive(handle, READ, 1, status) == HEADER + length && bytes_equal(HEADER, expected, length);
}

// The independent writer connects to this name only after closing its file handle.
bool wait_for_writer() {
    constexpr uint64_t parent = ABI_BOOTSTRAP_HANDLE;
    Endpoint peer;
    return send(parent, ABI_COORD_OP_REGISTER, 9, sys_stage(HEADER, WRITER_DONE)) &&
           receive(parent, ABI_COORD_OP_REGISTER, 9, 0) == HEADER &&
           receive(parent, ABI_COORD_OP_CONNECTION, 0, 0, &peer, STARTUP_TIMEOUT_NS) ==
               HEADER + sizeof(WRITER_DONE) - 1 &&
           bytes_equal(HEADER, WRITER_DONE, sizeof(WRITER_DONE) - 1);
}

bool writable_hello(uint64_t root) {
    Endpoint hello, upper, editor;
    if (!wait_for_writer() || !request_node(root, CREATE, "hello", APPEND_BYTES, 0, nullptr, ALREADY_EXISTS) ||
        !request_node(root, CREATE, "Hello", APPEND_BYTES, 0, &upper) || !stat(upper.handle, FILE, 0) ||
        !request_node(root, OPEN_RIGHTS, "hello", READ_BYTES, 0, &hello)) {
        return false;
    }
    sys_print("selftest: reading /hello from the writer\n");
    if (!read(hello.handle, 0, sizeof(HELLO), HELLO, sizeof(HELLO) - 1) ||
        sys_write(HEADER, sizeof(HELLO) - 1) != sizeof(HELLO) - 1) {
        return false;
    }
    return write(hello.handle, 0, "h", ACCESS_DENIED) &&
           request_node(root, OPEN_RIGHTS, "hello", READ_WRITE, 0, &editor) && write(editor.handle, 0, "h") &&
           read(hello.handle, 0, sizeof(HELLO), "hello, world!\n", sizeof(HELLO) - 1);
}

bool nested_nodes(uint64_t root) {
    Endpoint notes, drafts, today, reader;
    return request_node(root, CREATE, "notes", READ_WRITE | CREATE_CHILD, ALL_RIGHTS, &notes) &&
           write(notes.handle, 0, "My notebook") &&
           request_node(notes.handle, CREATE, "drafts", CREATE_CHILD, APPEND_BYTES, &drafts) &&
           request_node(drafts.handle, CREATE, "today", READ_WRITE, 0, nullptr, ACCESS_DENIED) &&
           request_node(drafts.handle, CREATE, "today", APPEND_BYTES, 0, &today) &&
           read(today.handle, 0, 1, "", 0, ACCESS_DENIED) && write(today.handle, APPEND_OFFSET, "day") &&
           write(today.handle, APPEND_OFFSET, " one") && write(today.handle, 0, "X", ACCESS_DENIED) &&
           request_node(root, OPEN_RIGHTS, "notes/drafts/today", READ_BYTES, 0, &reader) &&
           read(reader.handle, 0, 8, "day one", 7) && stat(notes.handle, DIRECTORY, 11) &&
           read(notes.handle, 0, 12, "My notebook", 11);
}

bool list_and_lookup(uint64_t root) {
    Endpoint list_only, lookup_only, append_ceiling, child;
    uint64_t cursor = 0;
    if (!request_node(root, OPEN_RIGHTS, "notes", LIST, READ_BYTES, &list_only) ||
        !listing(list_only.handle, "drafts", 6, DIRECTORY) ||
        !request_node(list_only.handle, OPEN_RIGHTS, "drafts", READ_BYTES, 0, nullptr, ACCESS_DENIED) ||
        !request_node(root, OPEN_RIGHTS, "notes", LOOKUP, READ_BYTES, &lookup_only)) {
        return false;
    }
    sys_copy_out(HEADER, &cursor, sizeof(cursor));
    return send(lookup_only.handle, READ_DIR, 1, sizeof(cursor)) &&
           receive(lookup_only.handle, READ_DIR, 1, ACCESS_DENIED) == HEADER &&
           request_node(lookup_only.handle, OPEN_RIGHTS, "drafts", READ_WRITE, 0, nullptr, ACCESS_DENIED) &&
           request_node(lookup_only.handle, OPEN_RIGHTS, "drafts", READ_BYTES, LOOKUP, nullptr, ACCESS_DENIED) &&
           request_node(lookup_only.handle, OPEN_RIGHTS, "drafts", READ_BYTES, 0, &child) &&
           request_node(root, OPEN_RIGHTS, "notes", LOOKUP, APPEND_BYTES, &append_ceiling) &&
           open(append_ceiling.handle, "drafts", nullptr, ACCESS_DENIED);
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
    if (!connect(root) || !open(root.handle, "boot", &boot) || !open(boot.handle, "data", &data) ||
        !listing(data.handle, FIXTURE_NAME, sizeof(FIXTURE_NAME) - 1, FILE) ||
        !open(data.handle, FIXTURE_NAME, &file)) {
        sys_print("selftest: file discovery or listing failed\n");
        return false;
    }

    if (!open(data.handle, "missing", nullptr, NOT_FOUND) ||
        !open(data.handle, "../bootstrap", nullptr, INVALID_PATH) ||
        !open(data.handle, "/boot", nullptr, INVALID_PATH) ||
        !open(data.handle, FIXTURE_NAME, nullptr, READ_ONLY, WRITE_ACCESS) ||
        !request_node(data.handle, CREATE, "new", READ_WRITE, 0, nullptr, READ_ONLY) ||
        !write(file.handle, 0, "X", READ_ONLY) || !open(file.handle, FIXTURE_NAME, nullptr, WRONG_KIND) ||
        !stat(file.handle, FILE, file_bytes) || !read(file.handle, 0, 3, FIXTURE_DATA, 3) ||
        !read(file.handle, 3, 100, FIXTURE_DATA + 3, file_bytes - 3) ||
        !read(file.handle, file_bytes, 1, FIXTURE_DATA, 0)) {
        sys_print("selftest: file reads or error replies failed\n");
        return false;
    }

    if (!print_hello(boot.handle)) {
        sys_print("selftest: /boot/hello.txt read or output failed\n");
        return false;
    }
    if (!writable_hello(root.handle)) {
        sys_print("selftest: two-client /hello failed\n");
        return false;
    }
    if (!nested_nodes(root.handle) || !list_and_lookup(root.handle)) {
        sys_print("selftest: nested node or permissions failed\n");
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
