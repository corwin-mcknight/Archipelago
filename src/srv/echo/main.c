#include <abi/syscall.h>
#include <stddef.h>
#include <stdint.h>
#include <sys.h>

// The echo server: the first program that is a server rather than a test payload. It multiplexes
// its two channels -- the parent mailbox on its bootstrap endpoint, and the client channel its
// creator endowed it with -- through one port, echoes client bytes back verbatim, prints parent
// mail, and exits when either peer hangs up. Launched without a client it still serves its
// mailbox, which is what makes it a shell-poke-able toy until task-kill exists.

#define KEY_PARENT 1
#define KEY_CLIENT 2

// IPC buffer layout. Staging for prints happens at offset 0; these stay clear of it.
#define ARRIVED_AT 512
#define PACKET_AT 640
#define MSG_AT 1024
#define MSG_CAP 2048

static uint64_t read_u64(size_t offset) {
    char* ipc    = sys_ipc_base();
    uint64_t out = 0;
    for (size_t i = 0; i < sizeof(uint64_t); i++) { out |= (uint64_t)(unsigned char)ipc[offset + i] << (8 * i); }
    return out;
}

// Print one received parent message as text: a staged prefix, the message bytes, a newline.
static void print_parent_mail(size_t length) {
    char* ipc      = sys_ipc_base();
    size_t at      = sys_stage(0, "echo: parent: ");
    size_t clamped = length;
    if (at + clamped + 1 > MSG_AT) { clamped = MSG_AT - at - 1; }
    for (size_t i = 0; i < clamped; i++) { ipc[at + i] = ipc[MSG_AT + i]; }
    ipc[at + clamped] = '\n';
    sys_write(0, at + clamped + 1);
}

// Drain a channel: every queued message through `consume`, stopping at the first recv failure --
// empty queue and hung-up-and-empty look the same here, and the signal poll afterwards is what
// distinguishes them. Returns 1 if the endpoint is exhausted for good (peer gone, queue empty).
static int drain(uint64_t channel, void (*consume)(uint64_t channel, size_t length)) {
    for (;;) {
        uint64_t got = sys_channel_recv(channel, MSG_AT, MSG_CAP, 0, 0);
        if (sys_is_error(got)) { break; }
        consume(channel, got & 0xFFFFFFFF);
    }
    uint64_t sig = sys_object_wait(channel, 0, 0);
    return (sig & ABI_CHANNEL_SIGNAL_PEER_CLOSED) != 0 && (sig & ABI_CHANNEL_SIGNAL_READABLE) == 0;
}

static void echo_back(uint64_t channel, size_t length) { sys_channel_send(channel, MSG_AT, length, 0, 0); }
static void show_mail(uint64_t channel, size_t length) {
    (void)channel;
    print_parent_mail(length);
}

int main(void) {
    uint64_t got = sys_channel_recv(ABI_BOOTSTRAP_HANDLE, 0, 64, ARRIVED_AT, 4);
    if (sys_is_error(got) || (got >> 32) < 2) {
        sys_print("echo: BOOTSTRAP BROKEN\n");
        return 1;
    }
    // Handles arrive in bootstrap order: self task, self thread, then the creator's extras -- for
    // this program, an optional client channel at [2].
    int have_client = (got >> 32) >= 3;
    uint64_t client = have_client ? read_u64(ARRIVED_AT + 2 * sizeof(uint64_t)) : 0;

    uint64_t port = sys_port_create();
    if (sys_is_error(port) ||
        sys_is_error(sys_port_bind(port, ABI_BOOTSTRAP_HANDLE, KEY_PARENT,
                                   ABI_CHANNEL_SIGNAL_READABLE | ABI_CHANNEL_SIGNAL_PEER_CLOSED)) ||
        (have_client && sys_is_error(sys_port_bind(port, client, KEY_CLIENT,
                                                   ABI_CHANNEL_SIGNAL_READABLE | ABI_CHANNEL_SIGNAL_PEER_CLOSED)))) {
        sys_print("echo: PORT SETUP BROKEN\n");
        return 1;
    }

    sys_print(have_client ? "echo: serving\n" : "echo: serving (mailbox only)\n");

    for (;;) {
        if (sys_is_error(sys_port_wait(port, PACKET_AT, 0))) {
            sys_print("echo: WAIT BROKEN\n");
            return 1;
        }
        uint64_t key = read_u64(PACKET_AT);

        if (key == KEY_CLIENT) {
            if (drain(client, echo_back)) {
                sys_print("echo: done\n");
                return 0;
            }
        } else if (key == KEY_PARENT) {
            if (drain(ABI_BOOTSTRAP_HANDLE, show_mail)) {
                sys_print("echo: orphaned\n");
                return 0;
            }
        }
    }
}
