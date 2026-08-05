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

    // The self-handles the kernel installed at creation, exercised through the dispatch pipeline.
    // Structural checks only -- type ids and rights bits are not part of the installed ABI yet, so
    // assert what the contract does promise: both self-handles resolve, they name objects of
    // different types, and an operation needing a right they lack (duplicate) is refused.
    uint64_t task_info   = init::obj_info(abi::syscall::SELF_TASK_HANDLE);
    uint64_t thread_info = init::obj_info(abi::syscall::SELF_THREAD_HANDLE);
    bool infos_ok        = !init::is_error(task_info) && !init::is_error(thread_info) &&
                    (task_info & 0xFFFFFFFF) != (thread_info & 0xFFFFFFFF);
    ipc.print(infos_ok ? "init: self handles ok\n" : "init: SELF HANDLES BROKEN\n");

    uint64_t dup = init::handle_duplicate(abi::syscall::SELF_TASK_HANDLE, ~0ull);
    ipc.print(init::is_error(dup) ? "init: rights check ok\n" : "init: RIGHTS CHECK MISSED\n");

    // A channel pair, exercised whole: create hands back two handles through the buffer (the first
    // kernel-to-user copy-out), a message sent on one end comes back byte-identical on the other,
    // a second recv correctly reports the queue empty, and both ends close.
    {
        constexpr size_t HANDLES_AT = 512;   // where create lands the two handles
        constexpr size_t REPLY_AT   = 768;   // where recv lands the message
        const char* ping            = "ping across the pair";

        bool ok = !init::is_error(init::channel_create(HANDLES_AT));
        uint64_t ends[2];
        for (size_t i = 0; i < 2 * sizeof(uint64_t); i++) {
            reinterpret_cast<char*>(ends)[i] = ipc.base[HANDLES_AT + i];
        }

        // Poll (zero mask): a fresh endpoint is writable and has nothing to read.
        uint64_t sig = init::object_wait(ends[0], 0);
        ok           = ok && (sig & abi::syscall::CHANNEL_SIGNAL_WRITABLE) != 0 &&
             (sig & abi::syscall::CHANNEL_SIGNAL_READABLE) == 0;

        size_t ping_len = ipc.stage(0, ping);
        ok              = ok && !init::is_error(init::channel_send(ends[0], 0, ping_len));

        // Wait for READABLE on the receiving end. The message is already queued, so this returns
        // immediately -- what it proves is the wait path itself observing the asserted signal.
        // Gated on ok: after an earlier failure the signal may never assert, and an unconditional
        // wait would hang forever instead of reporting CHANNEL BROKEN.
        if (ok) {
            sig = init::object_wait(ends[1], abi::syscall::CHANNEL_SIGNAL_READABLE);
            ok  = (sig & abi::syscall::CHANNEL_SIGNAL_READABLE) != 0;
        }

        uint64_t got = init::channel_recv(ends[1], REPLY_AT, 128);
        ok           = ok && got == ping_len;
        for (size_t i = 0; ok && i < ping_len; i++) { ok = ipc.base[REPLY_AT + i] == ping[i]; }

        // Drained queue: the error comes back immediately, the kernel never blocks for us.
        ok = ok && init::is_error(init::channel_recv(ends[1], REPLY_AT, 128));

        // Closing one end hangs up the other: PEER_CLOSED is already asserted by the time the
        // close returns, so this wait also cannot block (and is skipped after any failure, like
        // the READABLE wait above).
        ok = ok && !init::is_error(init::handle_close(ends[0]));
        if (ok) {
            sig = init::object_wait(ends[1], abi::syscall::CHANNEL_SIGNAL_PEER_CLOSED);
            ok  = (sig & abi::syscall::CHANNEL_SIGNAL_PEER_CLOSED) != 0;
        }

        ok = ok && !init::is_error(init::handle_close(ends[1]));
        ipc.print(ok ? "init: channel ok\n" : "init: CHANNEL BROKEN\n");
    }

    // Touch the demand-paged stack, then sleep so the lifecycle test sees a running task.
    volatile char stack_probe[64];
    for (size_t i = 0; i < sizeof(stack_probe); i++) { stack_probe[i] = static_cast<char>(i); }
    init::sleep(20);

    init::exit();
    __builtin_unreachable();
}
