#include <stddef.h>

#include "syscall.h"

// The first user program. It exists to exercise the path a real program takes -- ELF load, private
// address space, privilege transition, syscalls, exit -- so it deliberately touches memory in each
// kind of segment the loader has to get right, and writes through the IPC buffer the kernel handed
// it rather than through any pointer of its own.

namespace {

// .rodata: read-only, no execute.
const char* const GREETING = "hello from userspace\n";

// .data: initialised and writable, so a wrong permission or a missed copy shows up as a fault or as
// the wrong output rather than as silence.
char g_marker[] = "init: data segment intact\n";

// .bss: zero-filled by the loader through the anonymous VMO rather than copied from the image.
// volatile is load-bearing -- without it the compiler proves the array is all zeroes, folds the
// check below to `true`, and drops the array entirely, leaving the image with no .bss to test.
volatile char g_scratch[256];

bool bss_is_zero() {
    for (size_t i = 0; i < sizeof(g_scratch) / sizeof(g_scratch[0]); i++) {
        if (g_scratch[i] != '\0') { return false; }
    }
    return true;
}

}  // namespace

extern "C" [[noreturn]] void init_main(char* ipc_base, size_t ipc_size) {
    const init::ipc_buffer ipc{ipc_base, ipc_size};

    // Yield twice first: the scheduler has to bring us back before anything else is meaningful.
    init::yield();
    init::yield();

    ipc.print(GREETING);
    ipc.print(bss_is_zero() ? "init: bss zeroed\n" : "init: BSS NOT ZEROED\n");

    // Prove the data segment is writable, then read it back out through the same pointer.
    g_marker[0] = 'I';
    ipc.print(g_marker);

    // Two writes out of one staging pass, using the offset argument: the second names a slice the
    // first already staged, with no data movement in between.
    size_t first  = ipc.stage(0, "init: first half\n");
    size_t second = ipc.stage(first, "init: second half\n");
    ipc.write(0, first);
    ipc.write(first, second);

    // Touch the demand-paged stack, then sleep so the lifecycle test sees a running task.
    volatile char stack_probe[64];
    for (size_t i = 0; i < sizeof(stack_probe); i++) { stack_probe[i] = static_cast<char>(i); }
    init::sleep(20);

    init::exit();
    __builtin_unreachable();
}
