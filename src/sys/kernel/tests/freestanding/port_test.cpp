#include <kernel/obj/event.h>
#include <kernel/obj/port.h>
#include <kernel/sched/scheduler.h>
#include <kernel/testing/spawn.h>
#include <kernel/testing/testing.h>
#include <kernel/time.h>

using namespace kernel::obj;

KTEST_MODULE("kernel/port");

// The full wake chain with a real sleeper: a thread parked on the port is woken by a bound
// object's signal -- signal_set, port_notify, the port's own READABLE, wake_matching -- and the
// packet that arrives names the binding's key.
KTEST_CASE(port_wait_wakes_through_binding) {
    auto port  = ktl::make_ref<Port>();
    auto event = ktl::make_ref<Event>();
    KTEST_REQUIRE_TRUE(port && event);
    KTEST_REQUIRE_TRUE(port->bind(event, 9, 0b1).is_ok());

    auto signal_soon = [&] {
        kernel::sched::sleep_ticks(2);
        event->signal_set(0b1);
    };
    KTEST_REQUIRE_TRUE(kernel::testing::spawn_fn("port-signaler", signal_soon).is_ok());

    uint32_t got = port->wait_signals_deadline(Port::SIGNAL_READABLE, kernel::time::now() + 500);
    KTEST_REQUIRE_TRUE((got & Port::SIGNAL_READABLE) != 0);
    Port::Packet packet;
    KTEST_REQUIRE_TRUE(port->dequeue(packet));
    KTEST_EXPECT_ALL(packet.key == 9, packet.signals == 0b1);

    // And the timed side of the same wait: nothing further is coming, so a three-tick deadline
    // lapses instead of wedging the test.
    ktime_t before = kernel::time::now();
    got            = port->wait_signals_deadline(Port::SIGNAL_READABLE, before + 3);
    KTEST_EXPECT_TRUE((got & Port::SIGNAL_READABLE) == 0);
    KTEST_EXPECT_TRUE(kernel::time::now() - before >= 3);
    KTEST_EXPECT_TRUE(port->unbind(9) == 1);
}
