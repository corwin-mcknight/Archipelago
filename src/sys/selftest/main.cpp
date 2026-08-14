#include <abi/message.h>
#include <abi/syscall.h>
#include <stddef.h>
#include <stdint.h>
#include <sys.h>

// The user-mode self-check payload: exercises the path a real program takes -- ELF load, private
// address space, privilege transition, syscalls, exit -- so it deliberately touches memory in each
// kind of segment the loader has to get right, and writes through the IPC buffer the kernel handed
// it rather than through any pointer of its own. Its exit status is the test conduit: the low byte
// counts failed sections, and STATUS_ECHO_SKIPPED marks a run with no coordinator to connect
// through (spawned standalone by a kernel test), so watchers can tell "everything passed without a
// coordinator" from "everything passed including the echo roundtrip".

namespace {

constexpr uint32_t STATUS_ECHO_SKIPPED = 0x100;

// .rodata: read-only, no execute.
const char* const GREETING = "hello from userspace\n";

// .data: initialised and writable, so a wrong permission or a missed copy shows up as a fault or as
// the wrong output rather than as silence.
char g_marker[] = "selftest: data segment intact\n";

// .bss: zero-filled by the loader through the anonymous VMO rather than copied from the image.
// volatile is load-bearing -- without it the compiler proves the array is all zeroes, folds the
// check below to `true`, and drops the array entirely, leaving the image with no .bss to test.
volatile char g_scratch[256];

uint32_t g_failures = 0;

void report(bool ok, const char* good, const char* bad) {
    sys_print(ok ? good : bad);
    if (!ok) { g_failures++; }
}

bool bss_is_zero() {
    for (size_t i = 0; i < sizeof(g_scratch) / sizeof(g_scratch[0]); i++) {
        if (g_scratch[i] != '\0') { return false; }
    }
    return true;
}

#if defined(__x86_64__)
// SSE2 state across the kernel boundary, checked at the register level. The SysV ABI kills every
// xmm register at any call, so C code cannot hold a value in one across a syscall -- but the
// kernel must preserve whatever user code physically left there. One asm block loads a pattern,
// forces context switches with raw yield and sleep syscalls, and compares in place.
bool sse_state_survives() {
    alignas(16) static const uint64_t pattern[4] = {0x0123456789ABCDEFull, 0xFEDCBA9876543210ull,
                                                    0xA5A5A5A55A5A5A5Aull, 0x0F1E2D3C4B5A6978ull};
    uint32_t match;
    asm volatile(
        "movdqa (%[pat]), %%xmm7\n"
        "movdqa 16(%[pat]), %%xmm15\n"
        "movl %[yield], %%eax\n"
        "syscall\n"
        "movl %[sleep], %%eax\n"
        "movl $2, %%edi\n"
        "syscall\n"
        "movl %[yield], %%eax\n"
        "syscall\n"
        "pcmpeqb (%[pat]), %%xmm7\n"
        "pcmpeqb 16(%[pat]), %%xmm15\n"
        "pmovmskb %%xmm7, %%eax\n"
        "pmovmskb %%xmm15, %%edi\n"
        "andl %%edi, %%eax\n"
        : "=&a"(match)
        : [pat] "r"(pattern), [yield] "i"(ABI_SYS_YIELD), [sleep] "i"(ABI_SYS_SLEEP)
        : "rcx", "r11", "rdi", "xmm7", "xmm15", "cc", "memory");
    return match == 0xFFFF;
}
#endif

}  // namespace

// Freestanding C++ gives main no special treatment, so the C linkage the runtime links against
// must be spelled out.
extern "C" int main() {
    char* const ipc = sys_ipc_base();

    // Yield twice first: the scheduler has to bring us back before anything else is meaningful.
    sys_yield();
    sys_yield();

    sys_print(GREETING);
    report(bss_is_zero(), "selftest: bss zeroed\n", "selftest: BSS NOT ZEROED\n");

    // Prove the data segment is writable, then read it back out through the same pointer.
    g_marker[0] = 'S';
    sys_print(g_marker);

    // Two writes out of one staging pass, using the offset argument: the second names a slice the
    // first already staged, with no data movement in between.
    size_t first  = sys_stage(0, "selftest: first half\n");
    size_t second = sys_stage(first, "selftest: second half\n");
    sys_write(0, first);
    sys_write(first, second);

    // The bootstrap contract: slot 0 is a channel endpoint whose first message carries our
    // self-handles -- task, then thread. Recv it through the ordinary transfer path, then make the
    // structural checks the contract promises: an empty payload, at least the two self-handles,
    // naming objects of different types, and an operation needing a right they lack (duplicate)
    // refused.
    uint64_t self_task   = 0;
    uint64_t self_thread = 0;
    {
        constexpr size_t ARRIVED_AT = 512;  // where recv lands the endowed handles
        uint64_t got = sys_channel_recv(abi::syscall::BOOTSTRAP_HANDLE, 0, 64, ARRIVED_AT, 4);
        bool ok      = !sys_is_error(got) && (got & 0xFFFFFFFF) == 0 && (got >> 32) >= 2;
        sys_copy_in(&self_task, ARRIVED_AT, sizeof(self_task));
        sys_copy_in(&self_thread, ARRIVED_AT + sizeof(uint64_t), sizeof(self_thread));

        uint64_t task_info   = sys_obj_info(self_task);
        uint64_t thread_info = sys_obj_info(self_thread);
        ok                   = ok && !sys_is_error(task_info) && !sys_is_error(thread_info) &&
             (task_info & 0xFFFFFFFF) != (thread_info & 0xFFFFFFFF);

        // The endpoint must not report hangup after the drain: the parent keeps its end open for
        // the task's whole life. No claim about READABLE -- the parent may have mailed more already.
        uint64_t sig = sys_object_wait(abi::syscall::BOOTSTRAP_HANDLE, 0, 0);
        ok           = ok && (sig & abi::syscall::CHANNEL_SIGNAL_PEER_CLOSED) == 0;

        report(ok, "selftest: bootstrap ok\n", "selftest: BOOTSTRAP BROKEN\n");
    }

    uint64_t dup = sys_handle_duplicate(self_task, ~0ull);
    report(sys_is_error(dup), "selftest: rights check ok\n", "selftest: RIGHTS CHECK MISSED\n");

#if defined(__x86_64__)
    report(sse_state_survives(), "selftest: sse2 ok\n", "selftest: SSE2 BROKEN\n");
#endif

    // A channel pair, exercised whole: create hands back two handles through the buffer (the first
    // kernel-to-user copy-out), a message sent on one end comes back byte-identical on the other,
    // a second recv correctly reports the queue empty, and both ends close.
    {
        constexpr size_t HANDLES_AT = 512;  // where create lands the two handles
        constexpr size_t REPLY_AT   = 768;  // where recv lands the message
        const char* ping            = "ping across the pair";

        bool ok = !sys_is_error(sys_channel_create(HANDLES_AT));
        uint64_t ends[2];
        sys_copy_in(ends, HANDLES_AT, sizeof(ends));

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
        report(ok, "selftest: channel ok\n", "selftest: CHANNEL BROKEN\n");
    }

    // Handle transfer: an endpoint of a second pair rides a message across a first pair. The
    // sender's handle value dies with the send, the arrived value is new, and the transferred
    // endpoint still works -- a message sent through it lands on its peer.
    {
        constexpr size_t CARRIER_AT = 512;  // the pair the transfer travels over
        constexpr size_t CARGO_AT   = 528;  // the pair whose endpoint is transferred
        constexpr size_t SENT_AT    = 544;  // the handle value given to send
        constexpr size_t ARRIVED_AT = 552;  // where recv lands the transferred handle
        constexpr size_t REPLY_AT   = 768;
        const char* note            = "one endpoint enclosed";

        bool ok = !sys_is_error(sys_channel_create(CARRIER_AT)) && !sys_is_error(sys_channel_create(CARGO_AT));
        uint64_t carrier[2];
        uint64_t cargo[2];
        sys_copy_in(carrier, CARRIER_AT, sizeof(carrier));
        sys_copy_in(cargo, CARGO_AT, sizeof(cargo));

        size_t note_len = sys_stage(0, note);
        sys_copy_out(SENT_AT, &cargo[0], sizeof(cargo[0]));
        ok = ok && !sys_is_error(sys_channel_send(carrier[0], 0, note_len, SENT_AT, 1));

        // Consumed by the send: the old handle value must no longer resolve in our table.
        ok = ok && sys_is_error(sys_obj_info(cargo[0]));

        uint64_t got = sys_channel_recv(carrier[1], REPLY_AT, 128, ARRIVED_AT, 1);
        ok           = ok && !sys_is_error(got) && (got & 0xFFFFFFFF) == note_len && (got >> 32) == 1;

        uint64_t arrived = 0;
        sys_copy_in(&arrived, ARRIVED_AT, sizeof(arrived));

        // The arrived handle is a working channel endpoint: ping its peer through it.
        ok              = ok && !sys_is_error(sys_obj_info(arrived));
        size_t ping_len = sys_stage(0, "via transferred end");
        ok              = ok && !sys_is_error(sys_channel_send(arrived, 0, ping_len, 0, 0));
        ok              = ok && sys_channel_recv(cargo[1], REPLY_AT, 128, 0, 0) == ping_len;

        ok = ok && !sys_is_error(sys_handle_close(carrier[0])) && !sys_is_error(sys_handle_close(carrier[1]));
        ok = ok && !sys_is_error(sys_handle_close(arrived)) && !sys_is_error(sys_handle_close(cargo[1]));
        report(ok, "selftest: handle transfer ok\n", "selftest: HANDLE TRANSFER BROKEN\n");
    }

    // A port: the wait on an empty port times out rather than wedging, a message on a bound
    // channel becomes a packet carrying the binder's key, and unbind leaves the port empty.
    {
        constexpr size_t ENDS_AT   = 512;
        constexpr size_t PACKET_AT = 640;
        constexpr uint64_t KEY     = 0xC0FFEE;

        bool ok = !sys_is_error(sys_channel_create(ENDS_AT));
        uint64_t ends[2];
        sys_copy_in(ends, ENDS_AT, sizeof(ends));

        uint64_t port = sys_port_create();
        ok            = ok && !sys_is_error(port);
        ok = ok && !sys_is_error(sys_port_bind(port, ends[1], KEY, abi::syscall::CHANNEL_SIGNAL_READABLE));

        // Nothing queued yet: a bounded wait lapses instead of blocking forever.
        ok = ok && sys_port_wait(port, PACKET_AT, 1'000'000) == static_cast<uint64_t>(ABI_ERR_TIMED_OUT);

        size_t note_len = sys_stage(0, "wake the event loop");
        ok              = ok && !sys_is_error(sys_channel_send(ends[0], 0, note_len, 0, 0));
        ok              = ok && !sys_is_error(sys_port_wait(port, PACKET_AT, 0));

        uint64_t packet[2];
        sys_copy_in(packet, PACKET_AT, sizeof(packet));
        ok = ok && packet[0] == KEY && (packet[1] & abi::syscall::CHANNEL_SIGNAL_READABLE) != 0;

        ok = ok && sys_port_unbind(port, KEY) == 1;
        ok = ok && !sys_is_error(sys_handle_close(port));
        ok = ok && !sys_is_error(sys_handle_close(ends[0])) && !sys_is_error(sys_handle_close(ends[1]));
        report(ok, "selftest: port ok\n", "selftest: PORT BROKEN\n");
    }

    // The service layer, end to end: connect to "echo" by name through the coordinator (our
    // parent, on the bootstrap channel), then prove the minted channel reaches a live server in
    // another task. Spawned without a coordinator -- a kernel test driving this program directly --
    // no reply ever comes; the bounded wait turns that into the SKIPPED status rather than a hang
    // or a false failure.
    bool echo_skipped = false;
    {
        constexpr size_t MSG_AT    = 512;
        constexpr size_t ARRIVE_AT = 896;
        constexpr size_t REPLY_AT  = 960;
        constexpr uint64_t TXID    = 7;
        constexpr uint64_t WAIT_NS = 1'000'000'000;
        const char* pings[2]       = {"ping across tasks", "second opinion"};

        // CONNECT "echo": envelope, then the name as the rest of the payload.
        abi_message_header req{ABI_COORD_OP_CONNECT, 0, TXID};
        sys_copy_out(MSG_AT, &req, sizeof(req));
        size_t name_len = sys_stage(MSG_AT + sizeof(req), "echo");
        bool sent = !sys_is_error(sys_channel_send(abi::syscall::BOOTSTRAP_HANDLE, MSG_AT, sizeof(req) + name_len,
                                                   0, 0));

        // Await the matching reply; unrelated mail (there should be none) is skipped, not fatal.
        uint64_t peer = 0;
        bool ok       = sent;
        bool replied  = false;
        while (ok && !replied) {
            uint64_t sig = sys_object_wait(abi::syscall::BOOTSTRAP_HANDLE, abi::syscall::CHANNEL_SIGNAL_READABLE,
                                           WAIT_NS);
            if ((sig & abi::syscall::CHANNEL_SIGNAL_READABLE) == 0) { break; }
            uint64_t got = sys_channel_recv(abi::syscall::BOOTSTRAP_HANDLE, MSG_AT, 256, ARRIVE_AT, 1);
            if (sys_is_error(got)) { break; }
            if ((got & 0xFFFFFFFF) < sizeof(abi_message_header)) { continue; }
            abi_message_header reply;
            sys_copy_in(&reply, MSG_AT, sizeof(reply));
            if (reply.txid != TXID) { continue; }
            replied = true;
            ok      = reply.opcode == ABI_COORD_OP_CONNECT && reply.status == 0 && (got >> 32) == 1;
            sys_copy_in(&peer, ARRIVE_AT, sizeof(peer));
        }

        if (!replied) {
            echo_skipped = true;
            sys_print("selftest: echo skipped (no coordinator)\n");
        } else {
            for (size_t p = 0; ok && p < 2; p++) {
                size_t len   = sys_stage(0, pings[p]);
                ok           = !sys_is_error(sys_channel_send(peer, 0, len, 0, 0));
                uint64_t sig = ok ? sys_object_wait(peer, abi::syscall::CHANNEL_SIGNAL_READABLE, WAIT_NS) : 0;
                ok           = ok && (sig & abi::syscall::CHANNEL_SIGNAL_READABLE) != 0;
                ok           = ok && sys_channel_recv(peer, REPLY_AT, 128, 0, 0) == len;
                for (size_t i = 0; ok && i < len; i++) { ok = ipc[REPLY_AT + i] == pings[p][i]; }
            }
            ok = ok && !sys_is_error(sys_handle_close(peer));
            report(ok, "selftest: echo roundtrip ok\n", "selftest: ECHO ROUNDTRIP BROKEN\n");
        }
    }

    // Touch the demand-paged stack, then sleep so the lifecycle test sees a running task. Keep
    // the sleep well under user_task_lifecycle's 2000-tick termination wait: the two race, and a
    // sleep near that bound turns the test into a coin flip (as a temporary 2000 here proved).
    volatile char stack_probe[64];
    for (size_t i = 0; i < sizeof(stack_probe); i++) { stack_probe[i] = static_cast<char>(i); }
    sys_sleep(20);

    return static_cast<int>(g_failures | (echo_skipped ? STATUS_ECHO_SKIPPED : 0));
}
