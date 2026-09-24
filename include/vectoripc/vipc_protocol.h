#ifndef VECTORIPC_VIPC_PROTOCOL_H
#define VECTORIPC_VIPC_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VIPC_WIRE_MAJOR 1u
#define VIPC_WIRE_MINOR 0u
#define VIPC_PROTOCOL_NAME "VIPC"
#define VIPC_PROTOCOL_VERSION_STRING "VIPC/1.0"
#define VIPC_WIRE_HEADER_SIZE 32u
#define VIPC_MAX_PAYLOAD_BYTES (16u * 1024u * 1024u)
#define VIPC_APP_OPERATION_MIN 0x00000100u

/*
 * Public ABI scalar types are fixed-width. The constants remain ordinary
 * integral constant expressions while callers and structures never depend on
 * compiler-specific enum representation.
 */
typedef uint32_t vipc_message_kind;
enum {
    VIPC_KIND_REQUEST = 1u,
    VIPC_KIND_RESPONSE = 2u,
    VIPC_KIND_NOTIFY = 3u,
    VIPC_KIND_EVENT = 4u,
    VIPC_KIND_CONTROL = 5u
};

enum {
    VIPC_FLAG_NONE = 0u,
    VIPC_FLAG_ERROR = 1u << 0,
    VIPC_FLAG_KNOWN_MASK = VIPC_FLAG_ERROR
};

/*
 * Stable ABI v1 message descriptor. This is NOT the wire header; encode/decode
 * still map these fields explicitly into the fixed 32-byte little-endian wire
 * envelope.
 *
 * The order deliberately yields identical offsets under common 32/64-bit ABIs
 * and under externally changed structure packing.
 */
typedef struct vipc_message {
    uint32_t kind;
    uint32_t flags;
    uint32_t operation;
    uint32_t payload_size;
    uint64_t correlation_id;
} vipc_message;

typedef uint32_t vipc_protocol_status;
enum {
    VIPC_PROTOCOL_OK = 0u,
    VIPC_PROTOCOL_BAD_ARGUMENT = 1u,
    VIPC_PROTOCOL_BAD_MAGIC = 2u,
    VIPC_PROTOCOL_BAD_VERSION = 3u,
    VIPC_PROTOCOL_BAD_HEADER_SIZE = 4u,
    VIPC_PROTOCOL_BAD_KIND = 5u,
    VIPC_PROTOCOL_BAD_FLAGS = 6u,
    VIPC_PROTOCOL_PAYLOAD_TOO_LARGE = 7u
};

#if defined(__cplusplus)
static_assert(sizeof(vipc_message_kind) == 4u, "vipc_message_kind ABI width");
static_assert(sizeof(vipc_protocol_status) == 4u, "vipc_protocol_status ABI width");
static_assert(sizeof(vipc_message) == 24u, "vipc_message ABI size");
static_assert(offsetof(vipc_message, kind) == 0u, "vipc_message.kind ABI offset");
static_assert(offsetof(vipc_message, flags) == 4u, "vipc_message.flags ABI offset");
static_assert(offsetof(vipc_message, operation) == 8u, "vipc_message.operation ABI offset");
static_assert(offsetof(vipc_message, payload_size) == 12u, "vipc_message.payload_size ABI offset");
static_assert(offsetof(vipc_message, correlation_id) == 16u, "vipc_message.correlation_id ABI offset");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(vipc_message_kind) == 4u, "vipc_message_kind ABI width");
_Static_assert(sizeof(vipc_protocol_status) == 4u, "vipc_protocol_status ABI width");
_Static_assert(sizeof(vipc_message) == 24u, "vipc_message ABI size");
_Static_assert(offsetof(vipc_message, kind) == 0u, "vipc_message.kind ABI offset");
_Static_assert(offsetof(vipc_message, flags) == 4u, "vipc_message.flags ABI offset");
_Static_assert(offsetof(vipc_message, operation) == 8u, "vipc_message.operation ABI offset");
_Static_assert(offsetof(vipc_message, payload_size) == 12u, "vipc_message.payload_size ABI offset");
_Static_assert(offsetof(vipc_message, correlation_id) == 16u, "vipc_message.correlation_id ABI offset");
#endif

vipc_protocol_status vipc_protocol_validate(const vipc_message *message);

vipc_protocol_status vipc_protocol_encode(
    const vipc_message *message,
    uint8_t out_header[VIPC_WIRE_HEADER_SIZE]);

vipc_protocol_status vipc_protocol_decode(
    const uint8_t header[VIPC_WIRE_HEADER_SIZE],
    vipc_message *out_message);

const char *vipc_protocol_status_name(vipc_protocol_status status);

#ifdef __cplusplus
}
#endif

#endif
