#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING

#include <kernel/obj/event.h>
#include <kernel/obj/port.h>
#include <kernel/testing/test_objects.h>

using namespace kernel::testing;
using namespace kernel::obj;

KTEST_MODULE_WITH_INIT("obj/port", obj_port_init);

static void obj_port_init() {
    register_all_test_types();
    expect_registered(Port::register_type(g_type_registry), "Port test registration failed");
}

// The packet pipeline end to end: a bound object's signal transition queues a packet carrying the
// binder's key, READABLE tracks the queue, repeats before the dequeue coalesce, and dequeuing
// re-arms the binding for the next transition.
KTEST_CASE(obj_port_packet_delivery) {
    auto port  = ktl::make_ref<Port>();
    auto event = ktl::make_ref<Event>();
    KTEST_REQUIRE_TRUE(port && event);

    KTEST_REQUIRE_TRUE(port->bind(event, 0x5E55, 0b11).is_ok());
    KTEST_EXPECT_TRUE((port->signals() & Port::SIGNAL_READABLE) == 0);

    event->signal_set(0b01);
    KTEST_EXPECT_TRUE((port->signals() & Port::SIGNAL_READABLE) != 0);
    event->signal_set(0b10);  // coalesces into the still-pending packet

    Port::Packet packet;
    KTEST_REQUIRE_TRUE(port->dequeue(packet));
    KTEST_EXPECT_ALL(packet.key == 0x5E55, packet.signals == 0b11);
    KTEST_EXPECT_TRUE((port->signals() & Port::SIGNAL_READABLE) == 0);
    KTEST_EXPECT_FALSE(port->dequeue(packet));

    // Edge-triggered: the signal is still asserted, but without a fresh transition no second
    // packet appears. Clearing and re-setting is the transition.
    event->signal_set(0b01);
    KTEST_EXPECT_FALSE(port->dequeue(packet));
    event->signal_clear(0b11);
    event->signal_set(0b10);
    KTEST_REQUIRE_TRUE(port->dequeue(packet));
    KTEST_EXPECT_ALL(packet.key == 0x5E55, packet.signals == 0b10);
}

// A signal already inside the mask at bind time queues immediately -- binding a readable channel
// must see its existing messages -- and unbinding drops the binding along with any pending packet.
KTEST_CASE(obj_port_bind_asserted_and_unbind) {
    auto port  = ktl::make_ref<Port>();
    auto event = ktl::make_ref<Event>();
    KTEST_REQUIRE_TRUE(port && event);

    event->signal_set(0b100);
    KTEST_REQUIRE_TRUE(port->bind(event, 7, 0b100).is_ok());
    KTEST_EXPECT_TRUE((port->signals() & Port::SIGNAL_READABLE) != 0);

    KTEST_EXPECT_TRUE(port->unbind(7) == 1);
    KTEST_EXPECT_TRUE(port->binding_count() == 0);
    Port::Packet packet;
    KTEST_EXPECT_FALSE(port->dequeue(packet));
    KTEST_EXPECT_TRUE((port->signals() & Port::SIGNAL_READABLE) == 0);
    KTEST_EXPECT_TRUE(port->unbind(7) == 0);
}

// Bindings hold the object: dropping every handle-side reference leaves the object alive and
// still delivering until unbind, which is when the reference finally falls.
KTEST_CASE(obj_port_binding_pins_object) {
    auto port      = ktl::make_ref<Port>();
    bool destroyed = false;
    {
        auto watched = ktl::make_ref<TestObjA>(&destroyed);
        KTEST_REQUIRE_TRUE(port->bind(watched, 1, 0b1).is_ok());
    }
    KTEST_EXPECT_FALSE(destroyed);
    KTEST_EXPECT_TRUE(port->unbind(1) == 1);
    KTEST_EXPECT_TRUE(destroyed);
}

// Port teardown releases every binding: objects unpin and pending packets die with the port.
KTEST_CASE(obj_port_teardown_releases_bindings) {
    bool destroyed_a = false;
    bool destroyed_b = false;
    {
        auto port = ktl::make_ref<Port>();
        auto a    = ktl::make_ref<TestObjA>(&destroyed_a);
        auto b    = ktl::make_ref<TestObjA>(&destroyed_b);
        KTEST_REQUIRE_TRUE(port->bind(a, 1, 0b1).is_ok());
        KTEST_REQUIRE_TRUE(port->bind(b, 2, 0b1).is_ok());
        a.reset();
        b.reset();
        KTEST_EXPECT_ALL(!destroyed_a, !destroyed_b, port->binding_count() == 2);
    }
    KTEST_EXPECT_ALL(destroyed_a, destroyed_b);
}

// Two objects bound under different keys deliver in signal arrival order, each packet naming its
// own binder-chosen key -- the event-loop dispatch story.
KTEST_CASE(obj_port_two_sources_fifo) {
    auto port   = ktl::make_ref<Port>();
    auto first  = ktl::make_ref<Event>();
    auto second = ktl::make_ref<Event>();
    KTEST_REQUIRE_TRUE(port->bind(first, 100, 0b1).is_ok());
    KTEST_REQUIRE_TRUE(port->bind(second, 200, 0b1).is_ok());

    second->signal_set(0b1);
    first->signal_set(0b1);

    Port::Packet packet;
    KTEST_REQUIRE_TRUE(port->dequeue(packet));
    KTEST_EXPECT_TRUE(packet.key == 200);
    KTEST_REQUIRE_TRUE(port->dequeue(packet));
    KTEST_EXPECT_TRUE(packet.key == 100);
    KTEST_EXPECT_FALSE(port->dequeue(packet));
}

#endif
