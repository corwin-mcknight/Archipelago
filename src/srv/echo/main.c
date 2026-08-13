#include <abi/message.h>
#include <abi/syscall.h>
#include <stddef.h>
#include <stdint.h>
#include <sys.h>

// The echo server, as a real service: it registers the name "echo" with the coordinator (its
// parent, on the bootstrap channel) and serves every client the coordinator mints a connection
// for -- echoing client bytes back verbatim on each private channel. Clients come and go; the
// server exits only when its parent hangs up (orphaned) or registration is refused. Launched
// without a coordinator -- a kernel test spawning it directly -- the registration reply never
// comes and the event loop simply serves nothing, which is what makes it a good kill-test victim.

#define KEY_PARENT 1
#define KEY_CLIENT_BASE 0x100
#define MAX_CLIENTS 4
#define REGISTER_TXID 1

// IPC buffer layout. Staging for prints and sends happens at offset 0; these stay clear of it.
#define ARRIVE_AT 512
#define PACKET_AT 640
#define MSG_AT 1024
#define MSG_CAP 2048

static uint64_t g_port;
static uint64_t g_clients[MAX_CLIENTS];
static int g_client_used[MAX_CLIENTS];

static void copy_in(void* to, size_t offset, size_t length) {
    char* ipc = sys_ipc_base();
    for (size_t i = 0; i < length; i++) { ((char*)to)[i] = ipc[offset + i]; }
}

static void copy_out(size_t offset, const void* from, size_t length) {
    char* ipc = sys_ipc_base();
    for (size_t i = 0; i < length; i++) { ipc[offset + i] = ((const char*)from)[i]; }
}

// Drain a channel: every queued message through `consume`, stopping at the first recv failure --
// empty queue and hung-up-and-empty look the same here, and the signal poll afterwards is what
// distinguishes them. Returns 1 if the endpoint is exhausted for good (peer gone, queue empty).
static int drain(uint64_t channel, void (*consume)(uint64_t channel, uint64_t result)) {
    for (;;) {
        uint64_t got = sys_channel_recv(channel, MSG_AT, MSG_CAP, ARRIVE_AT, 1);
        if (sys_is_error(got)) { break; }
        consume(channel, got);
    }
    uint64_t sig = sys_object_wait(channel, 0, 0);
    return (sig & ABI_CHANNEL_SIGNAL_PEER_CLOSED) != 0 && (sig & ABI_CHANNEL_SIGNAL_READABLE) == 0;
}

static void echo_back(uint64_t channel, uint64_t result) {
    sys_channel_send(channel, MSG_AT, result & 0xFFFFFFFF, 0, 0);
}

// Parent mail: the registration reply, and CONNECTION messages carrying a new client's channel
// end. Anything else is ignored.
static void parent_mail(uint64_t channel, uint64_t result) {
    (void)channel;
    size_t size = result & 0xFFFFFFFF;
    if (size < sizeof(abi_message_header)) { return; }
    abi_message_header header;
    copy_in(&header, MSG_AT, sizeof(header));

    if (header.opcode == ABI_COORD_OP_REGISTER && header.txid == REGISTER_TXID) {
        sys_print(header.status == 0 ? "echo: registered\n" : "echo: REGISTER REFUSED\n");
        return;
    }
    if (header.opcode != ABI_COORD_OP_CONNECTION || (result >> 32) != 1) { return; }

    uint64_t client;
    copy_in(&client, ARRIVE_AT, sizeof(client));
    for (size_t i = 0; i < MAX_CLIENTS; i++) {
        if (g_client_used[i]) { continue; }
        if (sys_is_error(sys_port_bind(g_port, client, KEY_CLIENT_BASE + i,
                                       ABI_CHANNEL_SIGNAL_READABLE | ABI_CHANNEL_SIGNAL_PEER_CLOSED))) {
            break;
        }
        g_client_used[i] = 1;
        g_clients[i]     = client;
        sys_print("echo: client connected\n");
        return;
    }
    sys_print("echo: client refused (full)\n");
    sys_handle_close(client);
}

int main(void) {
    uint64_t got = sys_channel_recv(ABI_BOOTSTRAP_HANDLE, 0, 64, ARRIVE_AT, 4);
    if (sys_is_error(got) || (got >> 32) < 2) {
        sys_print("echo: BOOTSTRAP BROKEN\n");
        return 1;
    }

    g_port = sys_port_create();
    if (sys_is_error(g_port) ||
        sys_is_error(sys_port_bind(g_port, ABI_BOOTSTRAP_HANDLE, KEY_PARENT,
                                   ABI_CHANNEL_SIGNAL_READABLE | ABI_CHANNEL_SIGNAL_PEER_CLOSED))) {
        sys_print("echo: PORT SETUP BROKEN\n");
        return 1;
    }

    // Claim the name. The reply arrives through the event loop; a run with no coordinator simply
    // never hears back and serves nothing, forever.
    {
        abi_message_header reg = {ABI_COORD_OP_REGISTER, 0, REGISTER_TXID};
        copy_out(0, &reg, sizeof(reg));
        size_t name_len = sys_stage(sizeof(reg), "echo");
        sys_channel_send(ABI_BOOTSTRAP_HANDLE, 0, sizeof(reg) + name_len, 0, 0);
    }
    sys_print("echo: serving\n");

    for (;;) {
        if (sys_is_error(sys_port_wait(g_port, PACKET_AT, 0))) {
            sys_print("echo: WAIT BROKEN\n");
            return 1;
        }
        uint64_t key;
        copy_in(&key, PACKET_AT, sizeof(key));

        if (key == KEY_PARENT) {
            if (drain(ABI_BOOTSTRAP_HANDLE, parent_mail)) {
                sys_print("echo: orphaned\n");
                return 0;
            }
        } else if (key >= KEY_CLIENT_BASE && key < KEY_CLIENT_BASE + MAX_CLIENTS) {
            size_t slot = (size_t)(key - KEY_CLIENT_BASE);
            if (!g_client_used[slot]) { continue; }
            if (drain(g_clients[slot], echo_back)) {
                sys_port_unbind(g_port, key);
                sys_handle_close(g_clients[slot]);
                g_client_used[slot] = 0;
                sys_print("echo: client gone\n");
            }
        }
    }
}
