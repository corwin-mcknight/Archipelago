#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/obj/channel.h>
#include <kernel/testing/test_objects.h>

using namespace kernel::testing;
using namespace kernel::obj;

KTEST_MODULE_WITH_INIT("obj/channel", obj_channel_init);

static void obj_channel_init() {
    register_all_test_types();
    expect_registered(Channel::register_type(g_type_registry), "Channel test registration failed");
}

namespace {

MessageBuffer message_of(const char* s) {
    size_t n = 0;
    while (s[n] != '\0') { n++; }
    auto msg = MessageBuffer::create(n).unwrap();
    for (size_t i = 0; i < n; i++) { msg.data()[i] = static_cast<uint8_t>(s[i]); }
    return msg;
}

bool payload_equals(const MessageBuffer& m, const char* s) {
    size_t n = 0;
    while (s[n] != '\0') { n++; }
    if (m.size() != n) { return false; }
    for (size_t i = 0; i < n; i++) {
        if (m.data()[i] != static_cast<uint8_t>(s[i])) { return false; }
    }
    return true;
}

}  // namespace

// A fresh pair: both ends writable, neither readable, nobody closed, and each endpoint is a
// registered object the handle table will hold and type-check like any other.
KTEST_CASE(obj_channel_create_initial_state) {
    KTEST_UNWRAP(pair, Channel::create());
    KTEST_EXPECT_ALL(pair.first->signals() == Channel::SIGNAL_WRITABLE,
                     pair.second->signals() == Channel::SIGNAL_WRITABLE);

    HandleTable table;
    KTEST_UNWRAP(id, table.insert(pair.first, Channel::DEFAULT_RIGHTS));
    KTEST_UNWRAP(chan, table.get<Channel>(id));
    KTEST_EXPECT_TRUE(chan->type_id() == Channel::TYPE_ID);
    auto wrong = table.get<Event>(id);
    KTEST_EXPECT_ALL(wrong.is_err(), wrong.unwrap_err() == ktl::errc::wrong_type);

    // Endpoint handles are move-only: DUPLICATE is outside the type's valid rights, so a handle
    // carrying it cannot even be created.
    auto refused = table.insert(pair.second, Channel::DEFAULT_RIGHTS | RIGHT_DUPLICATE);
    KTEST_EXPECT_ALL(refused.is_err(), refused.unwrap_err() == ktl::errc::rights_violation);
    auto no_dup = table.duplicate(id, Channel::DEFAULT_RIGHTS);
    KTEST_EXPECT_ALL(no_dup.is_err(), no_dup.unwrap_err() == ktl::errc::rights_violation);

    KTEST_EXPECT_TRUE(table.close(id).is_ok());
}

// Messages cross in both directions, FIFO per direction, and the READABLE signal tracks the
// queue: set by the first write, held while a message remains, cleared by the final read.
KTEST_CASE(obj_channel_fifo_both_directions) {
    KTEST_UNWRAP(pair, Channel::create());

    KTEST_EXPECT_TRUE(pair.first->write(message_of("one")).is_ok());
    KTEST_EXPECT_TRUE(pair.first->write(message_of("two")).is_ok());
    KTEST_EXPECT_TRUE((pair.second->signals() & Channel::SIGNAL_READABLE) != 0);
    KTEST_EXPECT_TRUE((pair.first->signals() & Channel::SIGNAL_READABLE) == 0);

    KTEST_UNWRAP(m1, pair.second->read(64));
    KTEST_EXPECT_TRUE(payload_equals(m1, "one"));
    KTEST_EXPECT_TRUE((pair.second->signals() & Channel::SIGNAL_READABLE) != 0);
    KTEST_UNWRAP(m2, pair.second->read(64));
    KTEST_EXPECT_TRUE(payload_equals(m2, "two"));
    KTEST_EXPECT_TRUE((pair.second->signals() & Channel::SIGNAL_READABLE) == 0);

    KTEST_EXPECT_TRUE(pair.second->write(message_of("back")).is_ok());
    KTEST_UNWRAP(m3, pair.first->read(64));
    KTEST_EXPECT_TRUE(payload_equals(m3, "back"));
}

// An empty queue never blocks: read fails with would_block while the peer lives. A zero-length
// message is still a message, and an oversized write is rejected before touching the queue.
KTEST_CASE(obj_channel_empty_and_bounds) {
    KTEST_UNWRAP(pair, Channel::create());

    auto empty = pair.second->read(64);
    KTEST_EXPECT_ALL(empty.is_err(), empty.unwrap_err() == ktl::errc::would_block);

    KTEST_EXPECT_TRUE(pair.first->write(MessageBuffer{}).is_ok());
    KTEST_UNWRAP(nothing, pair.second->read(0));
    KTEST_EXPECT_TRUE(nothing.size() == 0);

    // Oversize is rejected where storage is granted, so no oversized message can ever exist.
    auto rejected = MessageBuffer::create(Channel::MAX_MESSAGE_BYTES + 1);
    KTEST_EXPECT_ALL(rejected.is_err(), rejected.unwrap_err() == ktl::errc::out_of_range);
}

// Filling the peer's queue clears the writer's WRITABLE and fails further writes immediately;
// draining one message re-asserts it and the write goes through again.
KTEST_CASE(obj_channel_full_queue_flow_control) {
    KTEST_UNWRAP(pair, Channel::create());

    for (size_t i = 0; i < Channel::QUEUE_DEPTH; i++) {
        KTEST_REQUIRE_TRUE(pair.first->write(message_of("m")).is_ok());
    }
    KTEST_EXPECT_TRUE((pair.first->signals() & Channel::SIGNAL_WRITABLE) == 0);

    auto refused = pair.first->write(message_of("overflow"));
    KTEST_EXPECT_ALL(refused.is_err(), refused.unwrap_err() == ktl::errc::capacity_exhausted);

    KTEST_EXPECT_TRUE(pair.second->read(64).is_ok());
    KTEST_EXPECT_TRUE((pair.first->signals() & Channel::SIGNAL_WRITABLE) != 0);
    KTEST_EXPECT_TRUE(pair.first->write(message_of("fits now")).is_ok());
}

// A message wider than the reader's capacity fails with truncated and stays queued, so the caller
// can retry with room; the retry delivers the same message intact.
KTEST_CASE(obj_channel_truncated_read_keeps_message) {
    KTEST_UNWRAP(pair, Channel::create());
    KTEST_REQUIRE_TRUE(pair.first->write(message_of("twelve bytes")).is_ok());

    auto small = pair.second->read(4);
    KTEST_EXPECT_ALL(small.is_err(), small.unwrap_err() == ktl::errc::truncated);
    KTEST_EXPECT_TRUE((pair.second->signals() & Channel::SIGNAL_READABLE) != 0);

    KTEST_UNWRAP(whole, pair.second->read(64));
    KTEST_EXPECT_TRUE(payload_equals(whole, "twelve bytes"));
}

// Dropping one endpoint: the survivor sees PEER_CLOSED and loses WRITABLE, writes fail with
// peer_closed, but messages already queued still drain -- and only then does read report
// peer_closed instead of would_block.
KTEST_CASE(obj_channel_peer_close) {
    KTEST_UNWRAP(pair, Channel::create());
    KTEST_REQUIRE_TRUE(pair.first->write(message_of("parting gift")).is_ok());

    pair.first = ktl::ref<Channel>();
    KTEST_EXPECT_TRUE((pair.second->signals() & Channel::SIGNAL_PEER_CLOSED) != 0);
    KTEST_EXPECT_TRUE((pair.second->signals() & Channel::SIGNAL_WRITABLE) == 0);

    auto refused = pair.second->write(message_of("into the void"));
    KTEST_EXPECT_ALL(refused.is_err(), refused.unwrap_err() == ktl::errc::peer_closed);

    KTEST_UNWRAP(last, pair.second->read(64));
    KTEST_EXPECT_TRUE(payload_equals(last, "parting gift"));
    auto drained = pair.second->read(64);
    KTEST_EXPECT_ALL(drained.is_err(), drained.unwrap_err() == ktl::errc::peer_closed);
}

#endif
