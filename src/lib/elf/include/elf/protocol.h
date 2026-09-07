#pragma once
#include <abi/message.h>
#include <sys.h>

// Private protocol between init and its ELF loader. All names are length-delimited.
// AUTHORITY: one TaskFactory handle, empty payload after the envelope.
// LOAD: one image VMO, abi_image_payload and name after the envelope.
// LOAD reply: envelope with matching txid, status, and task/mailbox handles on success.
#define ELF_LOADER_AUTHORITY 1u
#define ELF_LOADER_LOAD 2u

namespace elf {
inline uint64_t receive(uint64_t channel, uint64_t offset, uint64_t size, uint64_t handles, uint64_t capacity) {
    for (;;) {
        uint64_t result = sys_channel_recv(channel, offset, size, handles, capacity);
        if (result != static_cast<uint64_t>(ABI_ERR_WOULD_BLOCK)) { return result; }
        uint64_t waited = sys_object_wait(channel, ABI_CHANNEL_SIGNAL_READABLE | ABI_CHANNEL_SIGNAL_PEER_CLOSED, 0);
        if (sys_is_error(waited)) { return waited; }
    }
}
}  // namespace elf
