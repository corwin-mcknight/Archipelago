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
static int g_refused;

// Echo the bytes back; any handle a client attached is closed, not kept -- an echo reply carries
// bytes only, and an unclosed arrival would grow our handle table by one entry per message.
static void echo_back(void* ctx, uint64_t result) {
    sys_close_arrived(result, ARRIVE_AT);
    sys_channel_send(*(const uint64_t*)ctx, MSG_AT, result & 0xFFFFFFFF, 0, 0);
}

// Parent mail: the registration reply, and CONNECTION messages carrying a new client's channel
// end. Anything else is ignored, its stray handles closed.
static void parent_mail(void* ctx, uint64_t result) {
    (void)ctx;
    size_t size = result & 0xFFFFFFFF;
    abi_message_header header = {0, 0, 0};
    if (size >= sizeof(header)) { sys_copy_in(&header, MSG_AT, sizeof(header)); }

    if (header.opcode == ABI_COORD_OP_REGISTER && header.txid == REGISTER_TXID) {
        sys_close_arrived(result, ARRIVE_AT);
        if (header.status != 0) {
            sys_print("echo: REGISTER REFUSED\n");
            g_refused = 1;
            return;
        }
        sys_print("echo: registered\n");
        return;
    }
    if (header.opcode != ABI_COORD_OP_CONNECTION || (result >> 32) != 1) {
        sys_close_arrived(result, ARRIVE_AT);
        return;
    }

    uint64_t client;
    sys_copy_in(&client, ARRIVE_AT, sizeof(client));
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
        sys_copy_out(0, &reg, sizeof(reg));
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
        sys_copy_in(&key, PACKET_AT, sizeof(key));

        if (key == KEY_PARENT) {
            if (sys_channel_drain(ABI_BOOTSTRAP_HANDLE, MSG_AT, MSG_CAP, ARRIVE_AT, 4, parent_mail, 0)) {
                sys_print("echo: orphaned\n");
                return 0;
            }
            if (g_refused) { return 1; }
        } else if (key >= KEY_CLIENT_BASE && key < KEY_CLIENT_BASE + MAX_CLIENTS) {
            size_t slot = (size_t)(key - KEY_CLIENT_BASE);
            if (!g_client_used[slot]) { continue; }
            if (sys_channel_drain(g_clients[slot], MSG_AT, MSG_CAP, ARRIVE_AT, 1, echo_back, &g_clients[slot])) {
                sys_port_unbind(g_port, key);
                sys_handle_close(g_clients[slot]);
                g_client_used[slot] = 0;
                sys_print("echo: client gone\n");
            }
        }
    }
}
