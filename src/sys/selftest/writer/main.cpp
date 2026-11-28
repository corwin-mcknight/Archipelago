#include "../file_client.h"

// A separate bootstrap client. Connecting to the reader's rendezvous signals that
// the write completed and all of this process's filesystem handles have closed.
extern "C" int main() {
    using namespace file_test;
    const uint64_t got = sys_channel_recv(ABI_BOOTSTRAP_HANDLE, 0, HEADER, HANDLES_AT, 3);
    if (sys_is_error(got)) { return 1; }
    sys_close_arrived(got, HANDLES_AT);
    if (got != (uint64_t{3} << 32)) { return 1; }

    {
        Endpoint root, hello;
        if (!connect(root) || !listing(root.handle, "boot", 4, DIRECTORY) ||
            !open(root.handle, "hello", nullptr, NOT_FOUND) ||
            !request_node(root.handle, CREATE, "hello", APPEND_BYTES, 0, &hello) ||
            !write(hello.handle, APPEND_OFFSET, HELLO)) {
            sys_print("writer: /hello create or append failed\n");
            return 1;
        }
    }
    sys_print("writer: wrote and closed /hello\n");
    Endpoint reader;
    return connect(reader, WRITER_DONE) ? 0 : 1;
}
