#pragma once

#include <stdint.h>

// The system message envelope: the fixed first bytes of every message on a coordinator channel,
// and by convention on any service channel. Public ABI, installed beside <abi/syscall.h>.
//
// The envelope is what lets one channel carry many conversations. `opcode` says what the message
// is; `status` carries a reply's result (0 in requests); `txid` ties a reply to its request, so
// replies can arrive arbitrarily late and interleave with unsolicited messages without ambiguity.
// Unsolicited messages use txid 0.
//
// A per-opcode struct follows the envelope in the payload. Structs are packed native-endian --
// same-machine IPC on one architecture needs no serialization. Evolution is append-only: opcodes
// are never reused or renumbered, and a protocol grows by adding opcodes rather than versioning.
// Opcode spaces are per-protocol: the two peers of a channel agree on whose numbers apply, and
// nothing is global except this envelope.

typedef struct abi_message_header {
    uint32_t opcode;
    uint32_t status;
    uint64_t txid;
} abi_message_header;

#ifdef __cplusplus
static_assert(sizeof(abi_message_header) == 16, "the envelope is exactly two words");
#endif

// The coordinator protocol: messages on the bootstrap channel between a task and its parent.
// The kernel speaks IMAGE; the rest are the userspace coordinator's. Replies echo the request's
// opcode and txid, with status 0 for success or a negative error. Names are flat lowercase
// strings, never nul-terminated on the wire -- a name's length is whatever remains of the message
// after the fixed part.
//
// IMAGE (parent -> task, unsolicited): an executable image the receiver may spawn from. One VMO
// handle rides the message; the payload after the envelope is abi_image_payload followed by the
// image's name.
//
// REGISTER (task -> coordinator): claim a service name; the payload is the name. No handle -- a
// name is a claim, not an endpoint. The reply carries no payload. A registration lives exactly as
// long as the claimant: the coordinator drops a task's names when its mailbox hangs up.
//
// CONNECT (task -> coordinator): ask for a name; the payload is the name. The reply carries one
// handle, the requester's end of a freshly minted channel pair. A connect for an unregistered
// name parks until the name appears, so the reply may be arbitrarily late -- the txid is what
// makes it unambiguous.
//
// CONNECTION (coordinator -> registrant, unsolicited): a client connected to your name. The
// payload is the name; one handle rides the message, the server's end of the minted pair.
#define ABI_COORD_OP_IMAGE 1u
#define ABI_COORD_OP_REGISTER 2u
#define ABI_COORD_OP_CONNECT 3u
#define ABI_COORD_OP_CONNECTION 4u

typedef struct abi_image_payload {
    uint64_t size_bytes; /* exact image size; the VMO covers it rounded up to whole pages */
} abi_image_payload;

#ifdef __cplusplus
namespace abi::message {

using header                           = ::abi_message_header;
using image_payload                    = ::abi_image_payload;
constexpr uint32_t COORD_OP_IMAGE      = ABI_COORD_OP_IMAGE;
constexpr uint32_t COORD_OP_REGISTER   = ABI_COORD_OP_REGISTER;
constexpr uint32_t COORD_OP_CONNECT    = ABI_COORD_OP_CONNECT;
constexpr uint32_t COORD_OP_CONNECTION = ABI_COORD_OP_CONNECTION;

}  // namespace abi::message
#endif
