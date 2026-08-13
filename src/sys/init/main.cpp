#include <abi/message.h>
#include <abi/syscall.h>
#include <stddef.h>
#include <stdint.h>
#include <sys.h>

// The coordinator: the one task the kernel launches, and both process manager and service broker
// for everything else (docs/Design/Service Coordination.md). The kernel mails it one IMAGE
// message per boot module; it spawns each non-init image, becomes every child's parent, and
// serves the coordinator protocol on their mailboxes: REGISTER claims a name, CONNECT asks for
// one and receives an end of a freshly minted channel pair, the registrant receiving the other
// end as a CONNECTION message. Connects for absent names park until the name appears. Policy is
// open -- any child may claim any free name, every request is logged -- and the enforcement
// point, not the rules, is what this program establishes: every request arrives on a mailbox
// whose owner the coordinator spawned itself.

namespace {

// IPC buffer layout. Staging for sends at 0; the rest stay clear of it and each other.
constexpr size_t HANDLE_AT = 256;  // handle value given to send
constexpr size_t PAIR_AT   = 320;  // channel_create's two minted ends
constexpr size_t SPAWN_AT  = 384;  // task_spawn's task + mailbox handles
constexpr size_t PACKET_AT = 640;  // port packets
constexpr size_t ARRIVE_AT = 704;  // arrived handles from recv
constexpr size_t MSG_AT    = 1024; // recv payload landing
constexpr size_t MSG_CAP   = 2048;

constexpr uint64_t KEY_SELF        = 1;
constexpr uint64_t KEY_CHILD_BASE  = 0x100;
constexpr size_t MAX_CHILDREN      = 8;
constexpr size_t MAX_REGISTRATIONS = 8;
constexpr size_t MAX_PENDING       = 8;
constexpr size_t NAME_CAP          = 31;

struct child_slot {
    bool used;
    uint64_t task;
    uint64_t mailbox;
    char name[NAME_CAP];
    size_t name_len;
};
struct registration {
    bool used;
    size_t child;
    char name[NAME_CAP];
    size_t name_len;
};
struct pending_connect {
    bool used;
    size_t child;
    uint64_t txid;
    char name[NAME_CAP];
    size_t name_len;
};

child_slot g_children[MAX_CHILDREN];
registration g_names[MAX_REGISTRATIONS];
pending_connect g_pending[MAX_PENDING];
uint64_t g_port;

void copy_in(void* to, size_t offset, size_t length) {
    char* ipc = sys_ipc_base();
    for (size_t i = 0; i < length; i++) { static_cast<char*>(to)[i] = ipc[offset + i]; }
}

void copy_out(size_t offset, const void* from, size_t length) {
    char* ipc = sys_ipc_base();
    for (size_t i = 0; i < length; i++) { ipc[offset + i] = static_cast<const char*>(from)[i]; }
}

bool name_equal(const char* a, size_t a_len, const char* b, size_t b_len) {
    if (a_len != b_len) { return false; }
    for (size_t i = 0; i < a_len; i++) {
        if (a[i] != b[i]) { return false; }
    }
    return true;
}

// One log line built from a prefix and a length-delimited name: "coord: spawned echo\n".
void log_name(const char* prefix, const char* name, size_t name_len) {
    size_t at = sys_stage(0, prefix);
    copy_out(at, name, name_len);
    sys_ipc_base()[at + name_len] = '\n';
    sys_write(0, at + name_len + 1);
}

// Send an envelope, an optional name payload, and an optional handle on `channel`. The staged
// message starts at offset 0, so callers must be done with any staging of their own.
uint64_t send_message(uint64_t channel, uint32_t opcode, uint32_t status, uint64_t txid, const char* name,
                      size_t name_len, const uint64_t* handle) {
    abi_message_header header{opcode, status, txid};
    copy_out(0, &header, sizeof(header));
    if (name_len != 0) { copy_out(sizeof(header), name, name_len); }
    if (handle != nullptr) { copy_out(HANDLE_AT, handle, sizeof(*handle)); }
    return sys_channel_send(channel, 0, sizeof(header) + name_len, HANDLE_AT, handle != nullptr ? 1 : 0);
}

// Mint a pair and deliver both ends: CONNECTION (with the name) to the registrant's mailbox,
// then the reply (with the request's txid) to the requester. A failure to reach the server turns
// into an error reply; a failure to reach the requester is the requester's own death, and the
// minted ends die with this function either way -- send consumes them on success, close covers
// the rest.
void serve_connect(size_t requester, uint64_t txid, const char* name, size_t name_len, size_t server) {
    uint64_t ends[2];
    if (sys_is_error(sys_channel_create(PAIR_AT))) {
        (void)send_message(g_children[requester].mailbox, ABI_COORD_OP_CONNECT, static_cast<uint32_t>(-4), txid,
                           nullptr, 0, nullptr);
        return;
    }
    copy_in(ends, PAIR_AT, sizeof(ends));

    if (sys_is_error(send_message(g_children[server].mailbox, ABI_COORD_OP_CONNECTION, 0, 0, name, name_len,
                                  &ends[0]))) {
        (void)sys_handle_close(ends[0]);
        (void)sys_handle_close(ends[1]);
        (void)send_message(g_children[requester].mailbox, ABI_COORD_OP_CONNECT, static_cast<uint32_t>(-1), txid,
                           nullptr, 0, nullptr);
        return;
    }
    if (sys_is_error(send_message(g_children[requester].mailbox, ABI_COORD_OP_CONNECT, 0, txid, nullptr, 0,
                                  &ends[1]))) {
        (void)sys_handle_close(ends[1]);
    }
    log_name("coord: connected ", name, name_len);
}

// A name just appeared: serve every parked connect that was waiting for it.
void serve_pending(const char* name, size_t name_len, size_t server) {
    for (size_t i = 0; i < MAX_PENDING; i++) {
        if (!g_pending[i].used || !name_equal(g_pending[i].name, g_pending[i].name_len, name, name_len)) {
            continue;
        }
        g_pending[i].used = false;
        serve_connect(g_pending[i].child, g_pending[i].txid, name, name_len, server);
    }
}

// An IMAGE message from the kernel: spawn every image except our own. The VMO handle is consumed
// either way -- spawn only borrows it, and with no respawn story yet there is nothing to keep it
// for.
void handle_image(uint64_t vmo, size_t name_at, size_t name_len) {
    char name[NAME_CAP];
    if (name_len > NAME_CAP) { name_len = NAME_CAP; }
    copy_in(name, name_at, name_len);

    if (name_equal(name, name_len, "init", 4)) {
        (void)sys_handle_close(vmo);
        return;
    }

    size_t slot = MAX_CHILDREN;
    for (size_t i = 0; i < MAX_CHILDREN; i++) {
        if (!g_children[i].used) {
            slot = i;
            break;
        }
    }
    if (slot == MAX_CHILDREN || sys_is_error(sys_task_spawn(vmo, SPAWN_AT))) {
        log_name("coord: SPAWN FAILED ", name, name_len);
        (void)sys_handle_close(vmo);
        return;
    }
    (void)sys_handle_close(vmo);

    uint64_t handles[2];
    copy_in(handles, SPAWN_AT, sizeof(handles));
    g_children[slot].used     = true;
    g_children[slot].task     = handles[0];
    g_children[slot].mailbox  = handles[1];
    g_children[slot].name_len = name_len;
    for (size_t i = 0; i < name_len; i++) { g_children[slot].name[i] = name[i]; }

    if (sys_is_error(sys_port_bind(g_port, g_children[slot].mailbox, KEY_CHILD_BASE + slot,
                                   ABI_CHANNEL_SIGNAL_READABLE | ABI_CHANNEL_SIGNAL_PEER_CLOSED))) {
        log_name("coord: BIND FAILED ", name, name_len);
        return;
    }
    log_name("coord: spawned ", name, name_len);
}

void handle_child_message(size_t slot, uint64_t recv_result) {
    size_t size = recv_result & 0xFFFFFFFF;
    if (size < sizeof(abi_message_header)) { return; }
    abi_message_header header;
    copy_in(&header, MSG_AT, sizeof(header));
    size_t name_len = size - sizeof(header);
    if (name_len > NAME_CAP) { return; }
    char name[NAME_CAP];
    copy_in(name, MSG_AT + sizeof(header), name_len);

    if (header.opcode == ABI_COORD_OP_REGISTER) {
        for (size_t i = 0; i < MAX_REGISTRATIONS; i++) {
            if (g_names[i].used && name_equal(g_names[i].name, g_names[i].name_len, name, name_len)) {
                log_name("coord: register refused (taken) ", name, name_len);
                (void)send_message(g_children[slot].mailbox, ABI_COORD_OP_REGISTER, static_cast<uint32_t>(-7),
                                   header.txid, nullptr, 0, nullptr);
                return;
            }
        }
        size_t entry = MAX_REGISTRATIONS;
        for (size_t i = 0; i < MAX_REGISTRATIONS; i++) {
            if (!g_names[i].used) {
                entry = i;
                break;
            }
        }
        if (entry == MAX_REGISTRATIONS || name_len == 0) {
            (void)send_message(g_children[slot].mailbox, ABI_COORD_OP_REGISTER, static_cast<uint32_t>(-10),
                               header.txid, nullptr, 0, nullptr);
            return;
        }
        g_names[entry].used     = true;
        g_names[entry].child    = slot;
        g_names[entry].name_len = name_len;
        for (size_t i = 0; i < name_len; i++) { g_names[entry].name[i] = name[i]; }
        (void)send_message(g_children[slot].mailbox, ABI_COORD_OP_REGISTER, 0, header.txid, nullptr, 0, nullptr);
        log_name("coord: registered ", name, name_len);
        serve_pending(name, name_len, slot);
        return;
    }

    if (header.opcode == ABI_COORD_OP_CONNECT) {
        log_name("coord: connect ", name, name_len);
        for (size_t i = 0; i < MAX_REGISTRATIONS; i++) {
            if (g_names[i].used && name_equal(g_names[i].name, g_names[i].name_len, name, name_len)) {
                serve_connect(slot, header.txid, name, name_len, g_names[i].child);
                return;
            }
        }
        for (size_t i = 0; i < MAX_PENDING; i++) {
            if (g_pending[i].used) { continue; }
            g_pending[i].used     = true;
            g_pending[i].child    = slot;
            g_pending[i].txid     = header.txid;
            g_pending[i].name_len = name_len;
            for (size_t j = 0; j < name_len; j++) { g_pending[i].name[j] = name[j]; }
            log_name("coord: parked connect ", name, name_len);
            return;
        }
        (void)send_message(g_children[slot].mailbox, ABI_COORD_OP_CONNECT, static_cast<uint32_t>(-10), header.txid,
                           nullptr, 0, nullptr);
        return;
    }
    // Unknown opcodes are ignored: append-only evolution means an old coordinator may see new
    // requests, and dropping them beats guessing.
}

// A child's mailbox hung up: its registrations and parked connects die with it, and both handles
// close so the task object can too. Exit, crash, and kill all look identical here, by design.
void child_gone(size_t slot) {
    log_name("coord: child gone ", g_children[slot].name, g_children[slot].name_len);
    for (size_t i = 0; i < MAX_REGISTRATIONS; i++) {
        if (g_names[i].used && g_names[i].child == slot) { g_names[i].used = false; }
    }
    for (size_t i = 0; i < MAX_PENDING; i++) {
        if (g_pending[i].used && g_pending[i].child == slot) { g_pending[i].used = false; }
    }
    (void)sys_port_unbind(g_port, KEY_CHILD_BASE + slot);
    (void)sys_handle_close(g_children[slot].task);
    (void)sys_handle_close(g_children[slot].mailbox);
    g_children[slot].used = false;
}

// Drain a channel to exhaustion through `consume`; afterwards, report whether it is gone for good
// (hung up with nothing left to read).
template <typename F>
bool drain(uint64_t channel, F consume) {
    for (;;) {
        uint64_t got = sys_channel_recv(channel, MSG_AT, MSG_CAP, ARRIVE_AT, 4);
        if (sys_is_error(got)) { break; }
        consume(got);
    }
    uint64_t sig = sys_object_wait(channel, 0, 0);
    return (sig & ABI_CHANNEL_SIGNAL_PEER_CLOSED) != 0 && (sig & ABI_CHANNEL_SIGNAL_READABLE) == 0;
}

}  // namespace

extern "C" int main() {
    // Bootstrap: the self-handles. Nothing here needs them, but draining the first message is
    // what moves the mailbox to protocol traffic.
    uint64_t got = sys_channel_recv(abi::syscall::BOOTSTRAP_HANDLE, 0, 64, ARRIVE_AT, 4);
    if (sys_is_error(got) || (got >> 32) < 2) {
        sys_print("coord: BOOTSTRAP BROKEN\n");
        return 1;
    }

    g_port = sys_port_create();
    if (sys_is_error(g_port) ||
        sys_is_error(sys_port_bind(g_port, abi::syscall::BOOTSTRAP_HANDLE, KEY_SELF,
                                   ABI_CHANNEL_SIGNAL_READABLE | ABI_CHANNEL_SIGNAL_PEER_CLOSED))) {
        sys_print("coord: PORT SETUP BROKEN\n");
        return 1;
    }
    sys_print("coord: serving\n");

    for (;;) {
        if (sys_is_error(sys_port_wait(g_port, PACKET_AT, 0))) {
            sys_print("coord: WAIT BROKEN\n");
            return 1;
        }
        uint64_t key;
        copy_in(&key, PACKET_AT, sizeof(key));

        if (key == KEY_SELF) {
            // Our own mailbox: IMAGE messages from the kernel. A hangup here would mean the
            // kernel dropped our parent end, which it never does; keep serving regardless.
            (void)drain(abi::syscall::BOOTSTRAP_HANDLE, [](uint64_t result) {
                size_t size = result & 0xFFFFFFFF;
                if ((result >> 32) != 1 || size < sizeof(abi_message_header) + sizeof(abi_image_payload)) {
                    return;
                }
                abi_message_header header;
                copy_in(&header, MSG_AT, sizeof(header));
                if (header.opcode != ABI_COORD_OP_IMAGE) { return; }
                uint64_t vmo;
                copy_in(&vmo, ARRIVE_AT, sizeof(vmo));
                size_t fixed = sizeof(abi_message_header) + sizeof(abi_image_payload);
                handle_image(vmo, MSG_AT + fixed, size - fixed);
            });
        } else if (key >= KEY_CHILD_BASE && key < KEY_CHILD_BASE + MAX_CHILDREN) {
            size_t slot = static_cast<size_t>(key - KEY_CHILD_BASE);
            if (!g_children[slot].used) { continue; }
            bool gone = drain(g_children[slot].mailbox,
                              [slot](uint64_t result) { handle_child_message(slot, result); });
            if (gone) { child_gone(slot); }
        }
    }
}
