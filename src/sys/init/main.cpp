#include <abi/message.h>
#include <abi/syscall.h>
#include <elf/loader.h>
#include <elf/protocol.h>
#include <initrd/initrd.h>
#include <stddef.h>
#include <stdint.h>
#include <sys.h>

// The coordinator: the one task the kernel launches, and both process manager and service broker
// for everything else (docs/Design/Service Coordination.md). The kernel mails it one opaque
// initrd; it finds the bootstrap executables, becomes every child's parent, and
// serves the coordinator protocol on their mailboxes: REGISTER claims a name, CONNECT asks for
// one and receives an end of a freshly minted channel pair, the registrant receiving the other
// end as a CONNECTION message. Connects for absent names park until the name appears. Policy is
// open -- any child may claim any free name, every request is logged -- and the enforcement
// point, not the rules, is what this program establishes: every request arrives on a mailbox
// whose owner the coordinator spawned itself.

namespace {

// IPC buffer layout. Staging for sends at 0; the rest stay clear of it and each other.
constexpr size_t HANDLE_AT         = 256;   // handle value given to send
constexpr size_t PAIR_AT           = 320;   // channel_create's two minted ends
constexpr size_t SPAWN_AT          = 384;   // task_spawn's task + mailbox handles
constexpr size_t PACKET_AT         = 640;   // port packets
constexpr size_t ARRIVE_AT         = 704;   // arrived handles from recv
constexpr size_t MSG_AT            = 1024;  // recv payload landing
constexpr size_t MSG_CAP           = 2048;

constexpr uint64_t KEY_SELF        = 1;
constexpr uint64_t KEY_CHILD_BASE  = 0x100;
constexpr size_t MAX_CHILDREN      = 8;
constexpr size_t MAX_REGISTRATIONS = 8;
constexpr size_t MAX_PENDING       = 8;
constexpr size_t NAME_CAP          = ABI_COORD_NAME_MAX;

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
uint64_t g_factory;
uint64_t g_loader_task    = UINT64_MAX;
uint64_t g_loader_mailbox = UINT64_MAX;
uint64_t g_load_txid      = 0;
// Retain the original read-only blob for eventual handoff to the file server. Extracted image
// VMOs are temporary; they do not replace the archive's files and directories.
uint64_t g_initrd_vmo     = UINT64_MAX;
static_assert(initrd::BOOTSTRAP_NAME_MAX == NAME_CAP);

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
    sys_copy_out(at, name, name_len);
    sys_ipc_base()[at + name_len] = '\n';
    sys_write(0, at + name_len + 1);
}

// Send an envelope, an optional name payload, and an optional handle on `channel`. The staged
// message starts at offset 0, so callers must be done with any staging of their own.
uint64_t send_message(uint64_t channel, uint32_t opcode, uint32_t status, uint64_t txid, const char* name,
                      size_t name_len, const uint64_t* handle) {
    abi_message_header header{opcode, status, txid};
    sys_copy_out(0, &header, sizeof(header));
    if (name_len != 0) { sys_copy_out(sizeof(header), name, name_len); }
    if (handle != nullptr) { sys_copy_out(HANDLE_AT, handle, sizeof(*handle)); }
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
    sys_copy_in(ends, PAIR_AT, sizeof(ends));

    if (sys_is_error(
            send_message(g_children[server].mailbox, ABI_COORD_OP_CONNECTION, 0, 0, name, name_len, &ends[0]))) {
        (void)sys_handle_close(ends[0]);
        (void)sys_handle_close(ends[1]);
        (void)send_message(g_children[requester].mailbox, ABI_COORD_OP_CONNECT, static_cast<uint32_t>(-1), txid,
                           nullptr, 0, nullptr);
        return;
    }
    if (sys_is_error(
            send_message(g_children[requester].mailbox, ABI_COORD_OP_CONNECT, 0, txid, nullptr, 0, &ends[1]))) {
        (void)sys_handle_close(ends[1]);
    }
    log_name("coord: connected ", name, name_len);
}

// A name just appeared: serve every parked connect that was waiting for it.
void serve_pending(const char* name, size_t name_len, size_t server) {
    for (size_t i = 0; i < MAX_PENDING; i++) {
        if (!g_pending[i].used || !name_equal(g_pending[i].name, g_pending[i].name_len, name, name_len)) { continue; }
        g_pending[i].used = false;
        serve_connect(g_pending[i].child, g_pending[i].txid, name, name_len, server);
    }
}

// Transfer an image to the loader; the reply transfers ownership of the new child's task and
// parent bootstrap endpoint back to init. Applications therefore keep init as their coordinator.
uint64_t load_via_service(uint64_t vmo, uint64_t size, const char* name, size_t name_size) {
    abi_message_header request{ELF_LOADER_LOAD, 0, ++g_load_txid};
    abi_image_payload payload{size};
    sys_copy_out(0, &request, sizeof(request));
    sys_copy_out(sizeof(request), &payload, sizeof(payload));
    sys_copy_out(sizeof(request) + sizeof(payload), name, name_size);
    sys_copy_out(HANDLE_AT, &vmo, sizeof(vmo));
    uint64_t sent = sys_channel_send(g_loader_mailbox, 0, sizeof(request) + sizeof(payload) + name_size, HANDLE_AT, 1);
    // send may refuse before taking ownership. Closing the old generation is safe either way.
    (void)sys_handle_close(vmo);
    if (sys_is_error(sent)) { return sent; }
    uint64_t reply = elf::receive(g_loader_mailbox, 0, sizeof(abi_message_header), SPAWN_AT, 2);
    if (sys_is_error(reply)) { return reply; }
    abi_message_header response;
    sys_copy_in(&response, 0, sizeof(response));
    if ((reply & 0xffffffff) != sizeof(response) || response.opcode != ELF_LOADER_LOAD ||
        response.txid != request.txid || response.status != 0 || (reply >> 32) != 2) {
        sys_close_arrived(reply, SPAWN_AT);
        return static_cast<uint64_t>(ABI_ERR_INVALID_OPERATION);
    }
    return 0;
}

// Start an ordinary image through the loader. Both initrd bootstrap and explicit IMAGE mail use
// this path. It consumes the VMO on success and failure.
bool spawn_image(uint64_t vmo, uint64_t image_size, const char* name, size_t name_len) {
    if (name_len == 0 || name_len > NAME_CAP) {
        // Refused rather than truncated: a child spawned under a shortened identity would then
        // have its own full-name REGISTER refused, and two long module names could collide.
        sys_print("coord: INVALID IMAGE NAME; not spawned\n");
        (void)sys_handle_close(vmo);
        return false;
    }
    if (name_equal(name, name_len, "init", 4) || name_equal(name, name_len, "elf_loader", 10) ||
        g_loader_mailbox == UINT64_MAX) {
        (void)sys_handle_close(vmo);
        return false;
    }

    size_t slot = MAX_CHILDREN;
    for (size_t i = 0; i < MAX_CHILDREN; i++) {
        if (!g_children[i].used) {
            slot = i;
            break;
        }
    }
    if (slot == MAX_CHILDREN) {
        (void)sys_handle_close(vmo);
        return false;
    }
    if (sys_is_error(load_via_service(vmo, image_size, name, name_len))) {
        log_name("coord: SPAWN FAILED ", name, name_len);
        return false;
    }

    uint64_t handles[2];
    sys_copy_in(handles, SPAWN_AT, sizeof(handles));
    g_children[slot].used     = true;
    g_children[slot].task     = handles[0];
    g_children[slot].mailbox  = handles[1];
    g_children[slot].name_len = name_len;
    for (size_t i = 0; i < name_len; i++) { g_children[slot].name[i] = name[i]; }

    if (sys_is_error(sys_port_bind(g_port, g_children[slot].mailbox, KEY_CHILD_BASE + slot,
                                   ABI_CHANNEL_SIGNAL_READABLE | ABI_CHANNEL_SIGNAL_PEER_CLOSED))) {
        // An unbound child is unservable and its death unobservable; kill it and free the slot
        // rather than leaking both for the coordinator's lifetime.
        log_name("coord: BIND FAILED ", name, name_len);
        (void)sys_task_kill(g_children[slot].task);
        (void)sys_handle_close(g_children[slot].task);
        (void)sys_handle_close(g_children[slot].mailbox);
        g_children[slot].used = false;
        return false;
    }
    log_name("coord: spawned ", name, name_len);
    return true;
}

// The loader consumes images from VMO offset zero. Archive members need not start on a page,
// so give each one private, zero-padded backing and drop write authority before delivery.
uint64_t copy_image(const uint8_t* bytes, size_t size) {
    if (size == 0 || size > elf::MAX_IMAGE_BYTES) { return static_cast<uint64_t>(ABI_ERR_INVALID_OPERATION); }
    const uint64_t rounded = (size + ABI_VM_PAGE_SIZE - 1) & ~(ABI_VM_PAGE_SIZE - 1);
    uint64_t vmo           = sys_vmo_create(rounded);
    if (sys_is_error(vmo)) { return vmo; }
    uint64_t mapping = sys_vmo_map(vmo, 0, 0, rounded, ABI_VM_PROT_READ | ABI_VM_PROT_WRITE);
    if (sys_is_error(mapping)) {
        (void)sys_handle_close(vmo);
        return mapping;
    }
    auto* destination = reinterpret_cast<uint8_t*>(mapping);
    for (size_t i = 0; i < size; ++i) { destination[i] = bytes[i]; }
    uint64_t result = sys_vmo_unmap(mapping);
    if (!sys_is_error(result)) { result = sys_handle_restrict(vmo, ABI_RIGHT_READ); }
    if (sys_is_error(result)) {
        (void)sys_handle_close(vmo);
        return result;
    }
    return vmo;
}

bool start_loader(uint64_t vmo, size_t size) {
    uint64_t result = elf::spawn(g_factory, vmo, size, "elf_loader", 10, SPAWN_AT);
    (void)sys_handle_close(vmo);
    if (sys_is_error(result)) {
        sys_print("coord: LOADER BOOT FAILED\n");
        return false;
    }
    uint64_t handles[2];
    sys_copy_in(handles, SPAWN_AT, sizeof(handles));
    g_loader_task    = handles[0];
    g_loader_mailbox = handles[1];
    if (sys_is_error(send_message(g_loader_mailbox, ELF_LOADER_AUTHORITY, 0, 0, nullptr, 0, &g_factory))) {
        sys_print("coord: LOADER ENDOWMENT FAILED\n");
        return false;
    }
    g_factory = UINT64_MAX;  // construction authority now belongs only to the loader
    return true;
}

// A boot failure must not leave a partially started service environment behind. Init exits
// after this, which also releases any remaining self-handles and temporary mappings.
void stop_bootstrap() {
    for (size_t i = 0; i < MAX_CHILDREN; ++i) {
        if (!g_children[i].used) { continue; }
        (void)sys_task_kill(g_children[i].task);
        (void)sys_port_unbind(g_port, KEY_CHILD_BASE + i);
        (void)sys_handle_close(g_children[i].task);
        (void)sys_handle_close(g_children[i].mailbox);
        g_children[i].used = false;
    }
    if (g_loader_task != UINT64_MAX) {
        (void)sys_task_kill(g_loader_task);
        (void)sys_handle_close(g_loader_task);
        (void)sys_handle_close(g_loader_mailbox);
        g_loader_task = g_loader_mailbox = UINT64_MAX;
    }
}

bool bootstrap_archive(const void* bytes, size_t size) {
    initrd::Reader reader(bytes, size);
    struct BootstrapImage {
        const uint8_t* data;
        size_t size;
        size_t name_size;
        char name[NAME_CAP + 1];
    };
    BootstrapImage images[MAX_CHILDREN + 1]{};
    size_t count  = 0;
    size_t loader = MAX_CHILDREN + 1;
    initrd::Entry entry;
    // Finish validating and collecting the archive before starting any service.
    for (;;) {
        auto next = reader.next(entry);
        if (next == initrd::Result::END) { break; }
        if (next == initrd::Result::ERROR) {
            sys_print("coord: INVALID INITRD\n");
            return false;
        }
        // A regular file cannot occupy the root of the reserved bootstrap directory.
        if (!entry.directory && name_equal(entry.path, sizeof("bootstrap"), "bootstrap", sizeof("bootstrap"))) {
            sys_print("coord: INVALID INITRD BOOTSTRAP ROOT\n");
            return false;
        }
        if (!initrd::is_bootstrap_path(entry)) { continue; }
        char name[NAME_CAP + 1];
        if (count == MAX_CHILDREN + 1 || !initrd::bootstrap_name(entry, name)) {
            sys_print("coord: INVALID INITRD BOOTSTRAP SET\n");
            return false;
        }
        size_t name_size = 0;
        while (name[name_size] != '\0') { ++name_size; }
        if (name_equal(name, name_size, "init", 4)) {
            sys_print("coord: RESERVED INITRD BOOTSTRAP NAME\n");
            return false;
        }
        // Validate every executable before starting any of them; malformed late entries must
        // not leave earlier bootstrap services running in a half-initialized environment.
        if (elf::parse_image(entry.data, entry.size).is_err()) {
            log_name("coord: INVALID BOOTSTRAP ELF ", name, name_size);
            return false;
        }
        auto& image     = images[count];
        image.data      = entry.data;
        image.size      = entry.size;
        image.name_size = name_size;
        for (size_t i = 0; i <= name_size; ++i) { image.name[i] = name[i]; }
        if (name_equal(name, name_size, "elf_loader", 10)) { loader = count; }
        ++count;
    }
    if (loader == MAX_CHILDREN + 1) {
        sys_print("coord: INITRD LOADER MISSING\n");
        return false;
    }
    uint64_t loader_image = copy_image(images[loader].data, images[loader].size);
    if (sys_is_error(loader_image) || !start_loader(loader_image, images[loader].size)) { return false; }
    for (size_t i = 0; i < count; ++i) {
        if (i == loader) { continue; }
        uint64_t image = copy_image(images[i].data, images[i].size);
        if (sys_is_error(image) || !spawn_image(image, images[i].size, images[i].name, images[i].name_size)) {
            return false;
        }
    }
    return true;
}

// The second bootstrap message is exactly one opaque INITRD VMO. Archive parsing stays here,
// in userspace; the kernel knows only its exact byte extent and read-only content authority.
bool receive_initrd() {
    uint64_t got = elf::receive(abi::syscall::BOOTSTRAP_HANDLE, MSG_AT, MSG_CAP, ARRIVE_AT, 4);
    if (sys_is_error(got)) { return false; }
    constexpr size_t FIXED = sizeof(abi_message_header) + sizeof(abi_image_payload);
    if ((got >> 32) != 1 || (got & 0xffffffff) != FIXED) {
        sys_close_arrived(got, ARRIVE_AT);
        return false;
    }
    abi_message_header header;
    abi_image_payload payload;
    uint64_t vmo;
    sys_copy_in(&header, MSG_AT, sizeof(header));
    sys_copy_in(&payload, MSG_AT + sizeof(header), sizeof(payload));
    sys_copy_in(&vmo, ARRIVE_AT, sizeof(vmo));
    if (header.opcode != ABI_COORD_OP_INITRD || header.status != 0 || header.txid != 0 || payload.size_bytes == 0 ||
        payload.size_bytes > initrd::MAX_ARCHIVE_BYTES) {
        (void)sys_handle_close(vmo);
        return false;
    }
    const uint64_t rounded = (payload.size_bytes + ABI_VM_PAGE_SIZE - 1) & ~(ABI_VM_PAGE_SIZE - 1);
    uint64_t mapping       = sys_vmo_map(vmo, 0, 0, rounded, ABI_VM_PROT_READ);
    if (sys_is_error(mapping)) {
        (void)sys_handle_close(vmo);
        return false;
    }
    bool ready = bootstrap_archive(reinterpret_cast<const void*>(mapping), payload.size_bytes);
    if (sys_is_error(sys_vmo_unmap(mapping))) { ready = false; }
    if (!ready) {
        (void)sys_handle_close(vmo);
        return false;
    }
    g_initrd_vmo = vmo;
    sys_print("coord: initrd ready\n");
    return true;
}

void handle_child_message(size_t slot, uint64_t recv_result) {
    // No coordinator request carries a handle; whatever rode in must not squat in our table.
    sys_close_arrived(recv_result, ARRIVE_AT);
    size_t size = recv_result & 0xFFFFFFFF;
    if (size < sizeof(abi_message_header)) { return; }
    abi_message_header header;
    sys_copy_in(&header, MSG_AT, sizeof(header));
    size_t name_len = size - sizeof(header);
    if (name_len > NAME_CAP) {
        // Over the ABI bound: refuse with a reply rather than dropping silently, so a requester
        // blocked on the txid hears back instead of hanging forever.
        if (header.opcode == ABI_COORD_OP_REGISTER || header.opcode == ABI_COORD_OP_CONNECT) {
            (void)send_message(g_children[slot].mailbox, header.opcode, static_cast<uint32_t>(-10), header.txid,
                               nullptr, 0, nullptr);
        }
        return;
    }
    char name[NAME_CAP];
    sys_copy_in(name, MSG_AT + sizeof(header), name_len);

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
            (void)send_message(g_children[slot].mailbox, ABI_COORD_OP_REGISTER, static_cast<uint32_t>(-10), header.txid,
                               nullptr, 0, nullptr);
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

}  // namespace

extern "C" int main() {
    // Bootstrap: the self-handles. Nothing here needs them, but draining the first message is
    // what moves the mailbox to protocol traffic.
    uint64_t got = sys_channel_recv(abi::syscall::BOOTSTRAP_HANDLE, 0, 64, ARRIVE_AT, 4);
    if (sys_is_error(got) || (got >> 32) != 4) {
        sys_print("coord: BOOTSTRAP BROKEN\n");
        return 1;
    }

    sys_copy_in(&g_factory, ARRIVE_AT + 3 * sizeof(uint64_t), sizeof(g_factory));
    g_port = sys_port_create();
    if (sys_is_error(g_port) ||
        sys_is_error(sys_port_bind(g_port, abi::syscall::BOOTSTRAP_HANDLE, KEY_SELF,
                                   ABI_CHANNEL_SIGNAL_READABLE | ABI_CHANNEL_SIGNAL_PEER_CLOSED))) {
        sys_print("coord: PORT SETUP BROKEN\n");
        return 1;
    }
    if (!receive_initrd()) {
        sys_print("coord: INITRD BOOT FAILED\n");
        stop_bootstrap();
        return 1;
    }
    sys_print("coord: serving\n");

    for (;;) {
        if (sys_is_error(sys_port_wait(g_port, PACKET_AT, 0))) {
            sys_print("coord: WAIT BROKEN\n");
            return 1;
        }
        uint64_t key;
        sys_copy_in(&key, PACKET_AT, sizeof(key));

        if (key == KEY_SELF) {
            // Explicit executable delivery after boot. The initrd was consumed once before
            // entering this loop; a second archive cannot repeat bootstrap.
            (void)sys_channel_drain(
                abi::syscall::BOOTSTRAP_HANDLE, MSG_AT, MSG_CAP, ARRIVE_AT, 4,
                [](void*, uint64_t result) {
                    size_t size = result & 0xFFFFFFFF;
                    abi_message_header header{};
                    if ((result >> 32) == 1 && size >= sizeof(abi_message_header) + sizeof(abi_image_payload)) {
                        sys_copy_in(&header, MSG_AT, sizeof(header));
                    }
                    if (header.opcode != ABI_COORD_OP_IMAGE) {
                        // Malformed mail: whatever handles rode it must not squat in our table.
                        sys_close_arrived(result, ARRIVE_AT);
                        return;
                    }
                    uint64_t vmo;
                    sys_copy_in(&vmo, ARRIVE_AT, sizeof(vmo));
                    size_t fixed = sizeof(abi_message_header) + sizeof(abi_image_payload);
                    abi_image_payload payload;
                    sys_copy_in(&payload, MSG_AT + sizeof(abi_message_header), sizeof(payload));
                    size_t name_size = size - fixed;
                    if (name_size == 0 || name_size > NAME_CAP) {
                        (void)sys_handle_close(vmo);
                        return;
                    }
                    char name[NAME_CAP];
                    sys_copy_in(name, MSG_AT + fixed, name_size);
                    (void)spawn_image(vmo, payload.size_bytes, name, name_size);
                },
                nullptr);
        } else if (key >= KEY_CHILD_BASE && key < KEY_CHILD_BASE + MAX_CHILDREN) {
            size_t slot = static_cast<size_t>(key - KEY_CHILD_BASE);
            if (!g_children[slot].used) { continue; }
            bool gone =
                sys_channel_drain(
                    g_children[slot].mailbox, MSG_AT, MSG_CAP, ARRIVE_AT, 4,
                    [](void* ctx, uint64_t result) { handle_child_message(*static_cast<size_t*>(ctx), result); },
                    &slot) != 0;
            if (gone) { child_gone(slot); }
        }
    }
}
