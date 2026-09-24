#include "vectoripc/vipc_protocol.h"

enum {
    VIPC_WIRE_OFFSET_MAGIC = 0u,
    VIPC_WIRE_OFFSET_MAJOR = 4u,
    VIPC_WIRE_OFFSET_MINOR = 6u,
    VIPC_WIRE_OFFSET_HEADER_SIZE = 8u,
    VIPC_WIRE_OFFSET_KIND = 10u,
    VIPC_WIRE_OFFSET_FLAGS = 12u,
    VIPC_WIRE_OFFSET_OPERATION = 16u,
    VIPC_WIRE_OFFSET_CORRELATION = 20u,
    VIPC_WIRE_OFFSET_PAYLOAD_SIZE = 28u,
    VIPC_WIRE_PAYLOAD_SIZE_FIELD_BYTES = 4u
};

typedef char vipc_wire_layout_must_fill_header[
    (VIPC_WIRE_OFFSET_PAYLOAD_SIZE + VIPC_WIRE_PAYLOAD_SIZE_FIELD_BYTES
        == VIPC_WIRE_HEADER_SIZE)
        ? 1
        : -1];

static const uint8_t g_wire_magic[] = VIPC_PROTOCOL_NAME;
typedef char vipc_wire_magic_must_be_four_bytes[
    (sizeof(g_wire_magic) - 1u == 4u) ? 1 : -1];

static void put_u16_le(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value & 0xffu);
    p[1] = (uint8_t)((value >> 8) & 0xffu);
}

static void put_u32_le(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value & 0xffu);
    p[1] = (uint8_t)((value >> 8) & 0xffu);
    p[2] = (uint8_t)((value >> 16) & 0xffu);
    p[3] = (uint8_t)((value >> 24) & 0xffu);
}

static void put_u64_le(uint8_t *p, uint64_t value) {
    put_u32_le(p, (uint32_t)(value & UINT64_C(0xffffffff)));
    put_u32_le(p + 4, (uint32_t)(value >> 32));
}

static uint16_t get_u16_le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32_le(const uint8_t *p) {
    return (uint32_t)p[0]
        | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64_le(const uint8_t *p) {
    return (uint64_t)get_u32_le(p)
        | ((uint64_t)get_u32_le(p + 4) << 32);
}

static int kind_is_valid(uint32_t kind) {
    return kind >= (uint32_t)VIPC_KIND_REQUEST
        && kind <= (uint32_t)VIPC_KIND_CONTROL;
}

vipc_protocol_status vipc_protocol_validate(const vipc_message *message) {
    if (!message) return VIPC_PROTOCOL_BAD_ARGUMENT;
    if (!kind_is_valid(message->kind)) return VIPC_PROTOCOL_BAD_KIND;
    if ((message->flags & ~(uint32_t)VIPC_FLAG_KNOWN_MASK) != 0u) {
        return VIPC_PROTOCOL_BAD_FLAGS;
    }
    if (message->payload_size > VIPC_MAX_PAYLOAD_BYTES) {
        return VIPC_PROTOCOL_PAYLOAD_TOO_LARGE;
    }
    return VIPC_PROTOCOL_OK;
}

vipc_protocol_status vipc_protocol_encode(
    const vipc_message *message,
    uint8_t out_header[VIPC_WIRE_HEADER_SIZE]) {
    vipc_protocol_status status;
    size_t i;

    if (!message || !out_header) return VIPC_PROTOCOL_BAD_ARGUMENT;

    status = vipc_protocol_validate(message);
    if (status != VIPC_PROTOCOL_OK) return status;

    for (i = 0; i < VIPC_WIRE_HEADER_SIZE; ++i) out_header[i] = 0;

    for (i = 0u; i < sizeof(g_wire_magic) - 1u; ++i) {
        out_header[VIPC_WIRE_OFFSET_MAGIC + i] = g_wire_magic[i];
    }
    put_u16_le(out_header + VIPC_WIRE_OFFSET_MAJOR, (uint16_t)VIPC_WIRE_MAJOR);
    put_u16_le(out_header + VIPC_WIRE_OFFSET_MINOR, (uint16_t)VIPC_WIRE_MINOR);
    put_u16_le(
        out_header + VIPC_WIRE_OFFSET_HEADER_SIZE,
        (uint16_t)VIPC_WIRE_HEADER_SIZE);
    put_u16_le(out_header + VIPC_WIRE_OFFSET_KIND, (uint16_t)message->kind);
    put_u32_le(out_header + VIPC_WIRE_OFFSET_FLAGS, message->flags);
    put_u32_le(out_header + VIPC_WIRE_OFFSET_OPERATION, message->operation);
    put_u64_le(out_header + VIPC_WIRE_OFFSET_CORRELATION, message->correlation_id);
    put_u32_le(out_header + VIPC_WIRE_OFFSET_PAYLOAD_SIZE, message->payload_size);
    return VIPC_PROTOCOL_OK;
}

vipc_protocol_status vipc_protocol_decode(
    const uint8_t header[VIPC_WIRE_HEADER_SIZE],
    vipc_message *out_message) {
    uint16_t major;
    uint16_t minor;
    uint16_t header_size;
    vipc_message decoded;
    vipc_protocol_status status;

    if (!header || !out_message) return VIPC_PROTOCOL_BAD_ARGUMENT;

    {
        size_t i;
        for (i = 0u; i < sizeof(g_wire_magic) - 1u; ++i) {
            if (header[VIPC_WIRE_OFFSET_MAGIC + i] != g_wire_magic[i]) {
                return VIPC_PROTOCOL_BAD_MAGIC;
            }
        }
    }

    major = get_u16_le(header + VIPC_WIRE_OFFSET_MAJOR);
    minor = get_u16_le(header + VIPC_WIRE_OFFSET_MINOR);
    if (major != (uint16_t)VIPC_WIRE_MAJOR
        || minor > (uint16_t)VIPC_WIRE_MINOR) {
        return VIPC_PROTOCOL_BAD_VERSION;
    }

    header_size = get_u16_le(header + VIPC_WIRE_OFFSET_HEADER_SIZE);
    if (header_size != (uint16_t)VIPC_WIRE_HEADER_SIZE) {
        return VIPC_PROTOCOL_BAD_HEADER_SIZE;
    }

    decoded.kind = (uint32_t)get_u16_le(header + VIPC_WIRE_OFFSET_KIND);
    decoded.flags = get_u32_le(header + VIPC_WIRE_OFFSET_FLAGS);
    decoded.operation = get_u32_le(header + VIPC_WIRE_OFFSET_OPERATION);
    decoded.correlation_id = get_u64_le(header + VIPC_WIRE_OFFSET_CORRELATION);
    decoded.payload_size = get_u32_le(header + VIPC_WIRE_OFFSET_PAYLOAD_SIZE);

    status = vipc_protocol_validate(&decoded);
    if (status != VIPC_PROTOCOL_OK) return status;

    *out_message = decoded;
    return VIPC_PROTOCOL_OK;
}

const char *vipc_protocol_status_name(vipc_protocol_status status) {
    switch (status) {
        case VIPC_PROTOCOL_OK: return "ok";
        case VIPC_PROTOCOL_BAD_ARGUMENT: return "bad-argument";
        case VIPC_PROTOCOL_BAD_MAGIC: return "bad-magic";
        case VIPC_PROTOCOL_BAD_VERSION: return "bad-version";
        case VIPC_PROTOCOL_BAD_HEADER_SIZE: return "bad-header-size";
        case VIPC_PROTOCOL_BAD_KIND: return "bad-kind";
        case VIPC_PROTOCOL_BAD_FLAGS: return "bad-flags";
        case VIPC_PROTOCOL_PAYLOAD_TOO_LARGE: return "payload-too-large";
        default: return "unknown";
    }
}