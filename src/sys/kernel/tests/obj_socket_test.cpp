#include <kernel/obj/socket.h>
#include <kernel/sched/task.h>
#include <kernel/testing/test_objects.h>
#include <kernel/testing/testing.h>

using namespace kernel::testing;
using namespace kernel::obj;

KTEST_MODULE_WITH_INIT("obj/socket", obj_socket_init);

static void obj_socket_init() {
    register_all_test_types();
    expect_registered(Socket::register_type(g_type_registry), "Socket test registration failed");
}

namespace {

size_t write_str(const ktl::ref<Socket>& end, const char* s) {
    size_t n = 0;
    while (s[n] != '\0') { n++; }
    return end->write(s, n).unwrap();
}

bool read_equals(const ktl::ref<Socket>& end, const char* s) {
    size_t n = 0;
    while (s[n] != '\0') { n++; }
    char buf[128];
    auto got = end->read(buf, sizeof(buf));
    if (got.is_err() || got.unwrap() != n) { return false; }
    for (size_t i = 0; i < n; i++) {
        if (buf[i] != s[i]) { return false; }
    }
    return true;
}

}  // namespace

// A fresh pair: both ends writable, neither readable, nobody closed, and each endpoint is a
// registered object the handle table holds and type-checks like any other.
KTEST_CASE(obj_socket_create_initial_state) {
    KTEST_UNWRAP(pair, Socket::create());
    KTEST_EXPECT_ALL(pair.first->signals() == Socket::SIGNAL_WRITABLE,
                     pair.second->signals() == Socket::SIGNAL_WRITABLE);

    HandleTable table;
    KTEST_UNWRAP(id, table.insert(pair.first, Socket::DEFAULT_RIGHTS));
    KTEST_UNWRAP(sock, table.get<Socket>(id));
    KTEST_EXPECT_TRUE(sock->type_id() == Socket::TYPE_ID);
    KTEST_EXPECT_ERR(table.get<Event>(id), ktl::errc::wrong_type);

    // Direction is the handle's rights: an end inserted without the write right fails the same
    // verify the write syscall runs, while the full-rights handle passes.
    KTEST_UNWRAP(read_only, table.insert(pair.second, RIGHT_READ | RIGHT_WAIT));
    KTEST_EXPECT_ERR(table.verify(read_only, RIGHT_WRITE, Socket::TYPE_ID), ktl::errc::rights_violation);
    KTEST_EXPECT_TRUE(table.verify(id, RIGHT_WRITE, Socket::TYPE_ID).is_ok());
}

// Bytes flow in order with no boundaries: two writes drain as one read, each direction has its
// own stream, and the signal bits track buffered bytes.
KTEST_CASE(obj_socket_byte_stream_no_boundaries) {
    KTEST_UNWRAP(pair, Socket::create());

    KTEST_EXPECT_EQUAL(write_str(pair.first, "hello "), 6u);
    KTEST_EXPECT_EQUAL(write_str(pair.first, "stream"), 6u);
    KTEST_EXPECT_TRUE((pair.second->signals() & Socket::SIGNAL_READABLE) != 0);
    KTEST_EXPECT_TRUE((pair.first->signals() & Socket::SIGNAL_READABLE) == 0);

    KTEST_EXPECT_TRUE(read_equals(pair.second, "hello stream"));
    KTEST_EXPECT_TRUE((pair.second->signals() & Socket::SIGNAL_READABLE) == 0);

    // The opposite direction is independent.
    KTEST_EXPECT_EQUAL(write_str(pair.second, "back"), 4u);
    KTEST_EXPECT_TRUE(read_equals(pair.first, "back"));

    // A read caps at its capacity and leaves the rest buffered.
    KTEST_EXPECT_EQUAL(write_str(pair.first, "abcdef"), 6u);
    char buf[4];
    KTEST_UNWRAP(got, pair.second->read(buf, 2));
    KTEST_EXPECT_ALL(got == 2u, buf[0] == 'a', buf[1] == 'b');
    KTEST_EXPECT_TRUE(read_equals(pair.second, "cdef"));
}

// Backpressure: a write into a filling buffer shortens, a write into a full one fails, WRITABLE
// tracks the transitions, and draining reopens the flow. The ring wraps across the fill/drain
// cycle, so ordering is checked through the wrap seam too.
KTEST_CASE(obj_socket_backpressure_and_wrap) {
    KTEST_UNWRAP(pair, Socket::create());

    // Fill the buffer exactly: pattern writes until the short write lands on the boundary.
    uint8_t chunk[512];
    size_t total = 0;
    while (total < Socket::BUFFER_BYTES) {
        for (size_t i = 0; i < sizeof(chunk); i++) { chunk[i] = static_cast<uint8_t>((total + i) & 0xFF); }
        total += pair.first->write(chunk, sizeof(chunk)).unwrap();
    }
    KTEST_EXPECT_EQUAL(total, Socket::BUFFER_BYTES);
    KTEST_EXPECT_TRUE((pair.first->signals() & Socket::SIGNAL_WRITABLE) == 0);
    KTEST_EXPECT_ERR(pair.first->write(chunk, 1), ktl::errc::capacity_exhausted);

    // Drain a little: WRITABLE reasserts, and the next write wraps the ring.
    uint8_t out[600];
    KTEST_UNWRAP(drained, pair.second->read(out, sizeof(out)));
    KTEST_EXPECT_EQUAL(drained, sizeof(out));
    for (size_t i = 0; i < drained; i++) { KTEST_REQUIRE_TRUE(out[i] == static_cast<uint8_t>(i & 0xFF)); }
    KTEST_EXPECT_TRUE((pair.first->signals() & Socket::SIGNAL_WRITABLE) != 0);

    KTEST_UNWRAP(wrapped, pair.first->write(chunk, sizeof(chunk)));
    KTEST_EXPECT_EQUAL(wrapped, sizeof(chunk));

    // Everything still reads back in write order across the seam.
    size_t expect    = sizeof(out);
    size_t remaining = Socket::BUFFER_BYTES - sizeof(out) + sizeof(chunk);
    while (remaining > 0) {
        KTEST_UNWRAP(got, pair.second->read(out, sizeof(out)));
        for (size_t i = 0; i < got; i++) { KTEST_REQUIRE_TRUE(out[i] == static_cast<uint8_t>((expect + i) & 0xFF)); }
        expect += got;
        remaining -= got;
    }
    KTEST_EXPECT_ERR(pair.second->read(out, sizeof(out)), ktl::errc::would_block);
}

// Hangup: closing one end asserts PEER_CLOSED and kills the peer's writes immediately, but bytes
// already buffered stay readable -- peer_closed on read means empty for good, not closed.
KTEST_CASE(obj_socket_peer_close_semantics) {
    KTEST_UNWRAP(pair, Socket::create());

    KTEST_EXPECT_EQUAL(write_str(pair.first, "last words"), 10u);
    pair.first = ktl::ref<Socket>{};

    KTEST_EXPECT_TRUE((pair.second->signals() & Socket::SIGNAL_PEER_CLOSED) != 0);
    KTEST_EXPECT_TRUE((pair.second->signals() & Socket::SIGNAL_WRITABLE) == 0);
    KTEST_EXPECT_ERR(pair.second->write("x", 1), ktl::errc::peer_closed);

    KTEST_EXPECT_TRUE((pair.second->signals() & Socket::SIGNAL_READABLE) != 0);
    KTEST_EXPECT_TRUE(read_equals(pair.second, "last words"));
    char buf[8];
    KTEST_EXPECT_ERR(pair.second->read(buf, sizeof(buf)), ktl::errc::peer_closed);
}

KTEST_CASE(obj_socket_restrict_preserves_lifetime_and_hangup) {
    KTEST_UNWRAP(pair, Socket::create());
    HandleTable table;
    KTEST_UNWRAP(writer, table.insert(ktl::move(pair.first), Socket::DEFAULT_RIGHTS));
    KTEST_UNWRAP(reader, table.insert(ktl::move(pair.second), Socket::DEFAULT_RIGHTS));
    KTEST_REQUIRE_TRUE(table.restrict_rights(writer, RIGHT_WRITE | RIGHT_WAIT).is_ok());
    KTEST_REQUIRE_TRUE(table.restrict_rights(reader, RIGHT_READ | RIGHT_WAIT).is_ok());
    KTEST_EXPECT_EQUAL(table.count(), 2u);
    KTEST_EXPECT_ERR(table.get<Socket>(writer, RIGHT_READ), ktl::errc::rights_violation);
    KTEST_EXPECT_ERR(table.get<Socket>(reader, RIGHT_WRITE), ktl::errc::rights_violation);
    KTEST_EXPECT_ERR(table.duplicate(writer, RIGHT_WRITE), ktl::errc::rights_violation);
    KTEST_EXPECT_ERR(table.restrict_rights(writer, Socket::DEFAULT_RIGHTS), ktl::errc::rights_violation);
    {
        KTEST_UNWRAP(out, table.get<Socket>(writer, RIGHT_WRITE));
        KTEST_EXPECT_EQUAL(write_str(out, "buffered"), 8u);
    }
    KTEST_UNWRAP(in, table.get<Socket>(reader, RIGHT_READ));
    KTEST_EXPECT_TRUE((in->signals() & Socket::SIGNAL_PEER_CLOSED) == 0);
    KTEST_REQUIRE_TRUE(table.remove_rights(writer, UINT32_MAX).is_ok());
    KTEST_EXPECT_TRUE((in->signals() & Socket::SIGNAL_PEER_CLOSED) == 0);
    KTEST_REQUIRE_TRUE(table.close(writer).is_ok());
    KTEST_EXPECT_TRUE((in->signals() & Socket::SIGNAL_PEER_CLOSED) != 0);
    KTEST_EXPECT_TRUE(read_equals(in, "buffered"));
    char byte;
    KTEST_EXPECT_ERR(in->read(&byte, 1), ktl::errc::peer_closed);
}
