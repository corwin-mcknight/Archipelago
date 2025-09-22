#include <kernel/interrupt.h>
#include <kernel/testing/testing.h>
#include <kernel/x86/registers.h>

namespace {
constexpr unsigned kFunctionInterrupt = 45;
constexpr unsigned kObjectInterrupt = 46;
constexpr unsigned kClearedInterrupt = 47;

int g_function_handler_calls;
register_frame_t* g_function_handler_last_regs;

bool TestFunctionHandler(register_frame_t* regs) {
    ++g_function_handler_calls;
    g_function_handler_last_regs = regs;
    return true;
}

struct CountingHandler : kernel::hal::IInterruptHandler {
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
    frame.int_no = kFunctionInterrupt;

    g_interrupt_manager.register_interrupt(kFunctionInterrupt, &TestFunctionHandler, 0);
    g_interrupt_manager.dispatch_interrupt(kFunctionInterrupt, &frame);

    KTEST_EXPECT_EQUAL(g_function_handler_calls, 1);
    KTEST_EXPECT_TRUE(g_function_handler_last_regs == &frame);

    g_interrupt_manager.clear_handler(kFunctionInterrupt);
}

KTEST_WITH_INIT_INTEGRATION(InterruptManagerDispatchesObjectHandler, "x86_64/interrupts", InterruptManagerTestInit) {
    CountingHandler handler{};
    register_frame_t frame{};
    frame.int_no = kObjectInterrupt;

    g_interrupt_manager.register_interrupt(kObjectInterrupt, &handler, 0);
    g_interrupt_manager.dispatch_interrupt(kObjectInterrupt, &frame);

    KTEST_EXPECT_EQUAL(handler.call_count, 1);
    KTEST_EXPECT_TRUE(handler.last_regs == &frame);

    g_interrupt_manager.clear_handler(kObjectInterrupt);
}

KTEST_WITH_INIT_INTEGRATION(InterruptManagerClearDisablesHandler, "x86_64/interrupts", InterruptManagerTestInit) {
    register_frame_t frame{};
    frame.int_no = kClearedInterrupt;

    g_interrupt_manager.register_interrupt(kClearedInterrupt, &TestFunctionHandler, 0);
    g_interrupt_manager.dispatch_interrupt(kClearedInterrupt, &frame);

    KTEST_REQUIRE_EQUAL(g_function_handler_calls, 1);

    g_interrupt_manager.clear_handler(kClearedInterrupt);
    g_interrupt_manager.dispatch_interrupt(kClearedInterrupt, &frame);

    KTEST_EXPECT_EQUAL(g_function_handler_calls, 1);
}
