#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/obj/channel.h>
#include <kernel/sched/task.h>
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

// A message wider than the reader's capacity fails with truncated and is discarded -- leaving it
// queued would wedge the endpoint behind a message nobody can receive -- so the next read moves
// on to the next message, and READABLE clears once the queue is empty.
KTEST_CASE(obj_channel_truncated_read_discards_message) {
    KTEST_UNWRAP(pair, Channel::create());
    KTEST_REQUIRE_TRUE(pair.first->write(message_of("twelve bytes")).is_ok());
    KTEST_REQUIRE_TRUE(pair.first->write(message_of("next")).is_ok());

    auto small = pair.second->read(4);
    KTEST_EXPECT_ALL(small.is_err(), small.unwrap_err() == ktl::errc::truncated);
    KTEST_EXPECT_TRUE((pair.second->signals() & Channel::SIGNAL_READABLE) != 0);

    KTEST_UNWRAP(next, pair.second->read(64));
    KTEST_EXPECT_TRUE(payload_equals(next, "next"));
    KTEST_EXPECT_TRUE((pair.second->signals() & Channel::SIGNAL_READABLE) == 0);
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

// Handle escrow: a message owns kernel-table entries for handles in transit. They ride FIFO with
// the payload, a reader without room for them discards the message (closing its handles in the
// kernel table), and detaching hands them onward with rights intact and the kernel table back at
// its baseline.
KTEST_CASE(obj_channel_escrow_rides_message) {
    auto& escrow    = kernel::sched::kernel_task()->handles();
    size_t baseline = escrow.count();

    KTEST_UNWRAP(pair, Channel::create());
    bool lost = false;
    KTEST_UNWRAP(doomed, escrow.emplace<TestObjA>(RIGHT_READ, &lost));
    auto no_room = message_of("no room for cargo");
    KTEST_REQUIRE_TRUE(no_room.attach_handle(doomed));
    KTEST_REQUIRE_TRUE(pair.first->write(ktl::move(no_room)).is_ok());

    auto refused = pair.second->read(64, 0);
    KTEST_EXPECT_ALL(refused.is_err(), refused.unwrap_err() == ktl::errc::truncated);
    KTEST_EXPECT_ALL(lost, escrow.count() == baseline);
    KTEST_EXPECT_TRUE((pair.second->signals() & Channel::SIGNAL_READABLE) == 0);

    bool destroyed = false;
    KTEST_UNWRAP(cargo, escrow.emplace<TestObjA>(RIGHT_READ, &destroyed));
    auto msg = message_of("with cargo");
    KTEST_REQUIRE_TRUE(msg.attach_handle(cargo));
    KTEST_REQUIRE_TRUE(pair.first->write(ktl::move(msg)).is_ok());

    KTEST_UNWRAP(arrived, pair.second->read(64));
    KTEST_EXPECT_ALL(payload_equals(arrived, "with cargo"), arrived.handle_count() == 1);

    HandleId ids[MessageBuffer::MAX_HANDLES];
    KTEST_REQUIRE_TRUE(arrived.detach_handles(ids) == 1);
    KTEST_UNWRAP(taken, escrow.take(ids[0]));
    KTEST_EXPECT_ALL(taken.rights == RIGHT_READ, !destroyed, escrow.count() == baseline);
    taken.object.reset();
    KTEST_EXPECT_TRUE(destroyed);
}

// The channel dies with a handle-carrying message still queued: the kernel closes the escrowed
// handle normally -- no in-flight state survives the queue it rode in.
KTEST_CASE(obj_channel_escrow_closed_with_channel) {
    auto& escrow    = kernel::sched::kernel_task()->handles();
    size_t baseline = escrow.count();
    bool destroyed  = false;
    {
        KTEST_UNWRAP(pair, Channel::create());
        KTEST_UNWRAP(cargo, escrow.emplace<TestObjA>(RIGHT_READ, &destroyed));
        auto msg = message_of("never read");
        KTEST_REQUIRE_TRUE(msg.attach_handle(cargo));
        KTEST_REQUIRE_TRUE(pair.first->write(ktl::move(msg)).is_ok());
        KTEST_EXPECT_FALSE(destroyed);
    }
    KTEST_EXPECT_ALL(destroyed, escrow.count() == baseline);
}

#endif
