#include <kernel/testing/testing.h>

#if CONFIG_KERNEL_TESTING
#include <kernel/interrupt.h>
#include <kernel/x86/registers.h>

namespace {
constexpr unsigned kernel_test_interrupt_no = 45;

int g_function_handler_calls;
register_frame_t* g_function_handler_last_regs;

bool interrupt_test_function(register_frame_t* regs) {
    ++g_function_handler_calls;
    g_function_handler_last_regs = regs;
    return true;
}

struct counting_handler : kernel::hal::IInterruptHandler {
    bool return_value = true;
    int call_count = 0;
    register_frame_t* last_regs = nullptr;

    bool handle_interrupt(register_frame_t* regs) override {
        ++call_count;
        last_regs = regs;
        return return_value;
    }
};
}  // namespace

static void InterruptManagerTestInit() {
    g_interrupt_manager.initialize();
    g_function_handler_calls = 0;
    g_function_handler_last_regs = nullptr;
}

KTEST_WITH_INIT_INTEGRATION(InterruptManagerDispatchesFunctionHandler, "x86_64/interrupts", InterruptManagerTestInit) {
    register_frame_t frame{};
    frame.int_no = kernel_test_interrupt_no;

    g_interrupt_manager.register_interrupt(kernel_test_interrupt_no, &interrupt_test_function, 0);
    g_interrupt_manager.dispatch_interrupt(kernel_test_interrupt_no, &frame);

    KTEST_EXPECT_EQUAL(g_function_handler_calls, 1);
    KTEST_EXPECT_TRUE(g_function_handler_last_regs == &frame);

    g_interrupt_manager.clear_handler(kernel_test_interrupt_no);
}

KTEST_WITH_INIT_INTEGRATION(InterruptManagerDispatchesObjectHandler, "x86_64/interrupts", InterruptManagerTestInit) {
    counting_handler handler{};
    register_frame_t frame{};
    frame.int_no = kernel_test_interrupt_no;

    g_interrupt_manager.register_interrupt(kernel_test_interrupt_no, &handler, 0);
    g_interrupt_manager.dispatch_interrupt(kernel_test_interrupt_no, &frame);

    KTEST_EXPECT_EQUAL(handler.call_count, 1);
    KTEST_EXPECT_TRUE(handler.last_regs == &frame);

    g_interrupt_manager.clear_handler(kernel_test_interrupt_no);
}

KTEST_WITH_INIT_INTEGRATION(InterruptManagerClearDisablesHandler, "x86_64/interrupts", InterruptManagerTestInit) {
    register_frame_t frame{};
    frame.int_no = kernel_test_interrupt_no;

    g_interrupt_manager.register_interrupt(kernel_test_interrupt_no, &interrupt_test_function, 0);
    g_interrupt_manager.dispatch_interrupt(kernel_test_interrupt_no, &frame);

    KTEST_REQUIRE_EQUAL(g_function_handler_calls, 1);

    g_interrupt_manager.clear_handler(kernel_test_interrupt_no);
    g_interrupt_manager.dispatch_interrupt(kernel_test_interrupt_no, &frame);

    KTEST_EXPECT_EQUAL(g_function_handler_calls, 1);
}

#endif  // CONFIG_KERNEL_TESTING