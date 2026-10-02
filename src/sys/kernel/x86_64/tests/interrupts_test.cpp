#include <kernel/interrupt.h>
#include <kernel/testing/testing.h>
#include <kernel/time.h>
#include <kernel/x86/registers.h>

namespace {
constexpr unsigned kernel_test_interrupt_no = 45;
kernel::hal::interrupt_manager g_test_interrupt_manager;

int g_function_handler_calls;
register_frame_t* g_function_handler_last_regs;

bool interrupt_test_function(register_frame_t* regs) {
    ++g_function_handler_calls;
    g_function_handler_last_regs = regs;
    return true;
}

struct counting_handler : kernel::hal::IInterruptHandler {
    bool return_value           = true;
    int call_count              = 0;
    register_frame_t* last_regs = nullptr;

    bool handle_interrupt(register_frame_t* regs) override {
        ++call_count;
        last_regs = regs;
        return return_value;
    }
};
}  // namespace

KTEST_MODULE_WITH_INIT("x86_64/interrupts", interrupt_manager_test_init);

static void interrupt_manager_test_init() {
    g_test_interrupt_manager.initialize();
    g_function_handler_calls     = 0;
    g_function_handler_last_regs = nullptr;
}

// Initializing the dispatch fixture must preserve the live scheduler's timer handler.
KTEST_CASE(interrupt_manager_fixture_preserves_timer_handler) {
    const auto before = kernel::time::now();
    // Only the boot core advances global time; observe its real timer even if this test runs on an AP.
    for (unsigned spins = 0; kernel::time::now() == before && spins < 20'000'000; ++spins) { asm volatile("pause"); }
    KTEST_EXPECT_TRUE(kernel::time::now() > before);
}

// Function-handler lifecycle over one vector: register routes dispatch to the handler with the
// right frame, and clear_handler stops further delivery.
KTEST_CASE(interrupt_manager_function_handler_dispatch_and_clear) {
    register_frame_t frame{};
    frame.int_no = kernel_test_interrupt_no;

    g_test_interrupt_manager.register_interrupt(kernel_test_interrupt_no, &interrupt_test_function, 0);
    g_test_interrupt_manager.dispatch_interrupt(kernel_test_interrupt_no, &frame);

    KTEST_REQUIRE_EQUAL(g_function_handler_calls, 1);
    KTEST_EXPECT_TRUE(g_function_handler_last_regs == &frame);

    g_test_interrupt_manager.clear_handler(kernel_test_interrupt_no);
    g_test_interrupt_manager.dispatch_interrupt(kernel_test_interrupt_no, &frame);

    KTEST_EXPECT_EQUAL(g_function_handler_calls, 1);
}

KTEST_CASE(interrupt_manager_dispatches_object_handler) {
    counting_handler handler{};
    register_frame_t frame{};
    frame.int_no = kernel_test_interrupt_no;

    g_test_interrupt_manager.register_interrupt(kernel_test_interrupt_no, &handler, 0);
    g_test_interrupt_manager.dispatch_interrupt(kernel_test_interrupt_no, &frame);

    KTEST_EXPECT_EQUAL(handler.call_count, 1);
    KTEST_EXPECT_TRUE(handler.last_regs == &frame);

    g_test_interrupt_manager.clear_handler(kernel_test_interrupt_no);
}
