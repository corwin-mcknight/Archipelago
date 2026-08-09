#include <abi/syscall.h>
#include <stddef.h>
#include <stdint.h>
#include <sys.h>

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

// Freestanding C++ gives main no special treatment, so the C linkage the runtime links against
// must be spelled out.
extern "C" int main() {
    char* const ipc = sys_ipc_base();

    // Yield twice first: the scheduler has to bring us back before anything else is meaningful.
    sys_yield();
    sys_yield();

    sys_print(GREETING);
    sys_print(bss_is_zero() ? "init: bss zeroed\n" : "init: BSS NOT ZEROED\n");

    // Prove the data segment is writable, then read it back out through the same pointer.
    g_marker[0] = 'I';
    sys_print(g_marker);

    // Two writes out of one staging pass, using the offset argument: the second names a slice the
    // first already staged, with no data movement in between.
    size_t first  = sys_stage(0, "init: first half\n");
    size_t second = sys_stage(first, "init: second half\n");
    sys_write(0, first);
    sys_write(first, second);

    // The bootstrap contract: slot 0 is a channel endpoint whose first message carries our
    // self-handles -- task, then thread, then whatever else the creator endowed us with. Recv it
    // through the ordinary transfer path, then make the structural checks the contract promises:
    // an empty payload, at least the two self-handles, naming objects of different types, and an
    // operation needing a right they lack (duplicate) refused.
    uint64_t self_task   = 0;
    uint64_t self_thread = 0;
    {
        constexpr size_t ARRIVED_AT = 512;   // where recv lands the endowed handles
        uint64_t got = sys_channel_recv(abi::syscall::BOOTSTRAP_HANDLE, 0, 64, ARRIVED_AT, 4);
        bool ok      = !sys_is_error(got) && (got & 0xFFFFFFFF) == 0 && (got >> 32) >= 2;
        for (size_t i = 0; i < sizeof(uint64_t); i++) {
            reinterpret_cast<char*>(&self_task)[i]   = ipc[ARRIVED_AT + i];
            reinterpret_cast<char*>(&self_thread)[i] = ipc[ARRIVED_AT + sizeof(uint64_t) + i];
        }

        uint64_t task_info   = sys_obj_info(self_task);
        uint64_t thread_info = sys_obj_info(self_thread);
        ok                   = ok && !sys_is_error(task_info) && !sys_is_error(thread_info) &&
             (task_info & 0xFFFFFFFF) != (thread_info & 0xFFFFFFFF);

        // The endpoint must not report hangup after the drain: the parent keeps its end open for
        // the task's whole life. No claim about READABLE -- the parent may have mailed more already.
        uint64_t sig = sys_object_wait(abi::syscall::BOOTSTRAP_HANDLE, 0, 0);
        ok           = ok && (sig & abi::syscall::CHANNEL_SIGNAL_PEER_CLOSED) == 0;

        sys_print(ok ? "init: bootstrap ok\n" : "init: BOOTSTRAP BROKEN\n");
    }

    uint64_t dup = sys_handle_duplicate(self_task, ~0ull);
    sys_print(sys_is_error(dup) ? "init: rights check ok\n" : "init: RIGHTS CHECK MISSED\n");

    // A channel pair, exercised whole: create hands back two handles through the buffer (the first
    // kernel-to-user copy-out), a message sent on one end comes back byte-identical on the other,
    // a second recv correctly reports the queue empty, and both ends close.
    {
        constexpr size_t HANDLES_AT = 512;   // where create lands the two handles
        constexpr size_t REPLY_AT   = 768;   // where recv lands the message
        const char* ping            = "ping across the pair";

        bool ok = !sys_is_error(sys_channel_create(HANDLES_AT));
        uint64_t ends[2];
        for (size_t i = 0; i < 2 * sizeof(uint64_t); i++) { reinterpret_cast<char*>(ends)[i] = ipc[HANDLES_AT + i]; }

        // Poll (zero mask): a fresh endpoint is writable and has nothing to read.
        uint64_t sig = sys_object_wait(ends[0], 0, 0);
        ok           = ok && (sig & abi::syscall::CHANNEL_SIGNAL_WRITABLE) != 0 &&
             (sig & abi::syscall::CHANNEL_SIGNAL_READABLE) == 0;

        size_t ping_len = sys_stage(0, ping);
        ok              = ok && !sys_is_error(sys_channel_send(ends[0], 0, ping_len, 0, 0));

        // Wait for READABLE on the receiving end. The message is already queued, so this returns
        // immediately -- what it proves is the wait path itself observing the asserted signal.
        // Gated on ok: after an earlier failure the signal may never assert, and an unconditional
        // wait would hang forever instead of reporting CHANNEL BROKEN.
        if (ok) {
            sig = sys_object_wait(ends[1], abi::syscall::CHANNEL_SIGNAL_READABLE, 0);
            ok  = (sig & abi::syscall::CHANNEL_SIGNAL_READABLE) != 0;
        }

        uint64_t got = sys_channel_recv(ends[1], REPLY_AT, 128, 0, 0);
        ok           = ok && got == ping_len;
        for (size_t i = 0; ok && i < ping_len; i++) { ok = ipc[REPLY_AT + i] == ping[i]; }

        // Drained queue: the error comes back immediately, the kernel never blocks for us.
        ok = ok && sys_is_error(sys_channel_recv(ends[1], REPLY_AT, 128, 0, 0));

        // Closing one end hangs up the other: PEER_CLOSED is already asserted by the time the
        // close returns, so this wait also cannot block (and is skipped after any failure, like
        // the READABLE wait above).
        ok = ok && !sys_is_error(sys_handle_close(ends[0]));
        if (ok) {
            sig = sys_object_wait(ends[1], abi::syscall::CHANNEL_SIGNAL_PEER_CLOSED, 0);
            ok  = (sig & abi::syscall::CHANNEL_SIGNAL_PEER_CLOSED) != 0;
        }

        ok = ok && !sys_is_error(sys_handle_close(ends[1]));
        sys_print(ok ? "init: channel ok\n" : "init: CHANNEL BROKEN\n");
    }

    // Handle transfer: an endpoint of a second pair rides a message across a first pair. The
    // sender's handle value dies with the send, the arrived value is new, and the transferred
    // endpoint still works -- a message sent through it lands on its peer.
    {
        constexpr size_t CARRIER_AT = 512;   // the pair the transfer travels over
        constexpr size_t CARGO_AT   = 528;   // the pair whose endpoint is transferred
        constexpr size_t SENT_AT    = 544;   // the handle value given to send
        constexpr size_t ARRIVED_AT = 552;   // where recv lands the transferred handle
        constexpr size_t REPLY_AT   = 768;
        const char* note            = "one endpoint enclosed";

        bool ok = !sys_is_error(sys_channel_create(CARRIER_AT)) && !sys_is_error(sys_channel_create(CARGO_AT));
        uint64_t carrier[2];
        uint64_t cargo[2];
        for (size_t i = 0; i < 2 * sizeof(uint64_t); i++) {
            reinterpret_cast<char*>(carrier)[i] = ipc[CARRIER_AT + i];
            reinterpret_cast<char*>(cargo)[i]   = ipc[CARGO_AT + i];
        }

        size_t note_len = sys_stage(0, note);
        for (size_t i = 0; i < sizeof(uint64_t); i++) {
            ipc[SENT_AT + i] = reinterpret_cast<const char*>(&cargo[0])[i];
        }
        ok = ok && !sys_is_error(sys_channel_send(carrier[0], 0, note_len, SENT_AT, 1));

        // Consumed by the send: the old handle value must no longer resolve in our table.
        ok = ok && sys_is_error(sys_obj_info(cargo[0]));

        uint64_t got = sys_channel_recv(carrier[1], REPLY_AT, 128, ARRIVED_AT, 1);
        ok           = ok && !sys_is_error(got) && (got & 0xFFFFFFFF) == note_len && (got >> 32) == 1;

        uint64_t arrived = 0;
        for (size_t i = 0; i < sizeof(uint64_t); i++) { reinterpret_cast<char*>(&arrived)[i] = ipc[ARRIVED_AT + i]; }

        // The arrived handle is a working channel endpoint: ping its peer through it.
        ok              = ok && !sys_is_error(sys_obj_info(arrived));
        size_t ping_len = sys_stage(0, "via transferred end");
        ok              = ok && !sys_is_error(sys_channel_send(arrived, 0, ping_len, 0, 0));
        ok              = ok && sys_channel_recv(cargo[1], REPLY_AT, 128, 0, 0) == ping_len;

        ok = ok && !sys_is_error(sys_handle_close(carrier[0])) && !sys_is_error(sys_handle_close(carrier[1]));
        ok = ok && !sys_is_error(sys_handle_close(arrived)) && !sys_is_error(sys_handle_close(cargo[1]));
        sys_print(ok ? "init: handle transfer ok\n" : "init: HANDLE TRANSFER BROKEN\n");
    }

    // A port: the wait on an empty port times out rather than wedging, a message on a bound
    // channel becomes a packet carrying the binder's key, and unbind leaves the port empty.
    {
        constexpr size_t ENDS_AT   = 512;
        constexpr size_t PACKET_AT = 640;
        constexpr uint64_t KEY     = 0xC0FFEE;

        bool ok = !sys_is_error(sys_channel_create(ENDS_AT));
        uint64_t ends[2];
        for (size_t i = 0; i < 2 * sizeof(uint64_t); i++) { reinterpret_cast<char*>(ends)[i] = ipc[ENDS_AT + i]; }

        uint64_t port = sys_port_create();
        ok            = ok && !sys_is_error(port);
        ok = ok && !sys_is_error(sys_port_bind(port, ends[1], KEY, abi::syscall::CHANNEL_SIGNAL_READABLE));

        // Nothing queued yet: a bounded wait lapses instead of blocking forever. -15 is the
        // kernel's timed_out code; error values are not installed ABI yet (see todo.md), so this
        // is the one place init spells one.
        ok = ok && sys_port_wait(port, PACKET_AT, 1'000'000) == static_cast<uint64_t>(-15);

        size_t note_len = sys_stage(0, "wake the event loop");
        ok              = ok && !sys_is_error(sys_channel_send(ends[0], 0, note_len, 0, 0));
        ok              = ok && !sys_is_error(sys_port_wait(port, PACKET_AT, 0));

        uint64_t packet[2];
        for (size_t i = 0; i < 2 * sizeof(uint64_t); i++) { reinterpret_cast<char*>(packet)[i] = ipc[PACKET_AT + i]; }
        ok = ok && packet[0] == KEY && (packet[1] & abi::syscall::CHANNEL_SIGNAL_READABLE) != 0;

        ok = ok && sys_port_unbind(port, KEY) == 1;
        ok = ok && !sys_is_error(sys_handle_close(port));
        ok = ok && !sys_is_error(sys_handle_close(ends[0])) && !sys_is_error(sys_handle_close(ends[1]));
        sys_print(ok ? "init: port ok\n" : "init: PORT BROKEN\n");
    }

    // Touch the demand-paged stack, then sleep so the lifecycle test sees a running task. Keep
    // the sleep well under user_task_lifecycle's 2000-tick termination wait: the two race, and a
    // sleep near that bound turns the test into a coin flip (as a temporary 2000 here proved).
    volatile char stack_probe[64];
    for (size_t i = 0; i < sizeof(stack_probe); i++) { stack_probe[i] = static_cast<char>(i); }
    sys_sleep(20);

    return 0;
}
