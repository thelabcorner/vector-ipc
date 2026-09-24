#include "vipc_externalobject_abi.h"

#include "vectoripc/vipc.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define VIPC_EO_ADAPTER_VERSION 3u
#define VIPC_EO_MAX_PAYLOAD_BYTES (256u * 1024u)
#define VIPC_EO_PACK_BYTES 6u
#define VIPC_EO_PACK_BITS (VIPC_EO_PACK_BYTES * 8u)
#define VIPC_EO_MAX_PACKED ((UINT64_C(1) << VIPC_EO_PACK_BITS) - UINT64_C(1))
#define VIPC_EO_MAX_SESSIONS 16u
#define VIPC_EO_SLOT_BITS 4u
#define VIPC_EO_SLOT_MASK ((1u << VIPC_EO_SLOT_BITS) - 1u)
#define VIPC_EO_GENERATION_MAX (UINT32_MAX >> VIPC_EO_SLOT_BITS)

#if VIPC_EO_PACK_BYTES > 6u
#error "ExternalObject packed numerics must stay within the exact 48-bit double lane"
#endif
#if VIPC_EO_MAX_PAYLOAD_BYTES > VIPC_MAX_PAYLOAD_BYTES
#error "ExternalObject payload cap cannot exceed the VectorIPC wire cap"
#endif
#if VIPC_EO_MAX_SESSIONS > (1u << VIPC_EO_SLOT_BITS)
#error "VIPC_EO_SLOT_BITS cannot represent every ExternalObject session slot"
#endif

enum vipc_eo_command {
    VIPC_EO_CMD_INFO = 0,
    VIPC_EO_CMD_CONNECT = 1,
    VIPC_EO_CMD_STAGE_RESET = 2,
    VIPC_EO_CMD_STAGE_APPEND = 3,
    VIPC_EO_CMD_TRANSACT = 4,
    VIPC_EO_CMD_SEND = 5,
    VIPC_EO_CMD_RECEIVE = 6,
    VIPC_EO_CMD_CLOSE = 7,
    VIPC_EO_CMD_TRANSACT_TEXT = 8
};

typedef struct vipc_eo_session {
    uint32_t generation;
    int in_use;
    vipc_channel *channel;
    uint8_t *stage;
    uint32_t stage_size;
    uint32_t stage_capacity;
    uint8_t *receive;
    uint32_t receive_capacity;
} vipc_eo_session;

static vipc_eo_session g_sessions[VIPC_EO_MAX_SESSIONS];

#ifdef _WIN32
static volatile LONG g_busy;
#endif

static void set_undefined(vipc_es_tagged_data *retval) {
    if (!retval) return;
    retval->data.string = NULL;
    retval->type = VIPC_ES_TYPE_UNDEFINED;
    retval->filler = 0;
}

static int set_string_owned(
    vipc_es_tagged_data *retval,
    char *owned_string) {
    if (!retval || !owned_string) {
        free(owned_string);
        return 0;
    }
    retval->data.string = owned_string;
    retval->type = VIPC_ES_TYPE_STRING;
    retval->filler = 0;
    return 1;
}

static char *duplicate_string(const char *text) {
    size_t length;
    char *copy;
    if (!text) return NULL;
    length = strlen(text);
    copy = (char *)malloc(length + 1u);
    if (!copy) return NULL;
    memcpy(copy, text, length + 1u);
    return copy;
}

static char *format_simple(const char *format, ...) {
    va_list args;
    va_list copy;
    int needed;
    char *buffer;

    va_start(args, format);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return NULL;
    }

    buffer = (char *)malloc((size_t)needed + 1u);
    if (!buffer) {
        va_end(args);
        return NULL;
    }

    (void)vsnprintf(buffer, (size_t)needed + 1u, format, args);
    va_end(args);
    return buffer;
}

static char *format_error(
    vipc_status status,
    const vipc_error *error) {
    vipc_phase phase = error ? error->phase : VIPC_PHASE_NONE;
    uint32_t platform = error ? error->platform_code : 0u;
    return format_simple(
        VIPC_PROTOCOL_VERSION_STRING "|ERR|%u|%u|%lu",
        (unsigned)status,
        (unsigned)phase,
        (unsigned long)platform);
}

static int numeric_to_u32(
    const vipc_es_tagged_data *value,
    uint32_t *out) {
    double number;

    if (!value || !out) return 0;

    if (value->type == VIPC_ES_TYPE_UINTEGER) {
        *out = (uint32_t)value->data.intval;
        return 1;
    }
    if (value->type == VIPC_ES_TYPE_INTEGER) {
        if (value->data.intval < 0) return 0;
        *out = (uint32_t)value->data.intval;
        return 1;
    }
    if (value->type != VIPC_ES_TYPE_DOUBLE) return 0;

    number = value->data.fltval;
    if (!isfinite(number)
        || number < 0.0
        || number > 4294967295.0
        || floor(number) != number) {
        return 0;
    }
    *out = (uint32_t)number;
    return 1;
}

static int numeric_to_u48(
    const vipc_es_tagged_data *value,
    uint64_t *out) {
    double number;
    uint32_t small;

    if (!value || !out) return 0;

    if (value->type == VIPC_ES_TYPE_INTEGER
        || value->type == VIPC_ES_TYPE_UINTEGER) {
        if (!numeric_to_u32(value, &small)) return 0;
        *out = (uint64_t)small;
        return 1;
    }
    if (value->type != VIPC_ES_TYPE_DOUBLE) return 0;

    number = value->data.fltval;
    if (!isfinite(number)
        || number < 0.0
        || number > (double)VIPC_EO_MAX_PACKED
        || floor(number) != number) {
        return 0;
    }
    *out = (uint64_t)number;
    return 1;
}

static uint64_t join_u64(uint32_t low, uint32_t high) {
    return (uint64_t)low | ((uint64_t)high << 32);
}

static uint32_t low_u32(uint64_t value) {
    return (uint32_t)(value & UINT64_C(0xffffffff));
}

static uint32_t high_u32(uint64_t value) {
    return (uint32_t)(value >> 32);
}

static uint32_t packed_count_for_bytes(uint32_t byte_count) {
    return byte_count / VIPC_EO_PACK_BYTES
        + (byte_count % VIPC_EO_PACK_BYTES != 0u ? 1u : 0u);
}

#ifdef _WIN32
static uint32_t remaining_timeout_ms(
    ULONGLONG started,
    uint32_t total_timeout_ms) {
    ULONGLONG elapsed = GetTickCount64() - started;
    if (elapsed >= (ULONGLONG)total_timeout_ms) return 0u;
    return total_timeout_ms - (uint32_t)elapsed;
}
#endif

static int reserve_buffer(
    uint8_t **buffer,
    uint32_t *capacity,
    uint32_t required) {
    uint32_t next;
    uint8_t *resized;

    if (required <= *capacity) return 1;
    if (required > VIPC_EO_MAX_PAYLOAD_BYTES) return 0;

    next = *capacity ? *capacity : 4096u;
    while (next < required) {
        if (next > VIPC_EO_MAX_PAYLOAD_BYTES / 2u) {
            next = VIPC_EO_MAX_PAYLOAD_BYTES;
            break;
        }
        next *= 2u;
    }

    resized = (uint8_t *)realloc(*buffer, next);
    if (!resized) return 0;
    *buffer = resized;
    *capacity = next;
    return 1;
}

static int unpack_bytes(
    const vipc_es_tagged_data *packed,
    uint32_t packed_count,
    uint8_t *out,
    uint32_t byte_count) {
    uint32_t i;
    uint32_t written = 0;

    if (packed_count != packed_count_for_bytes(byte_count)) return 0;
    if (packed_count != 0u && !packed) return 0;
    if (byte_count != 0u && !out) return 0;

    for (i = 0; i < packed_count; ++i) {
        uint64_t value;
        uint32_t j;
        if (!numeric_to_u48(&packed[i], &value)) return 0;

        for (j = 0; j < VIPC_EO_PACK_BYTES && written < byte_count; ++j) {
            out[written++] = (uint8_t)(value & UINT64_C(0xff));
            value >>= 8;
        }
    }

    return written == byte_count;
}

static char *base64_frame(
    const vipc_message *message,
    const uint8_t *payload,
    uint32_t payload_size) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char prefix[192];
    int prefix_length;
    size_t encoded_length;
    size_t total_length;
    char *output;
    char *cursor;
    uint32_t i = 0;

    if (!message || (payload_size != 0u && !payload)) return NULL;

    prefix_length = snprintf(
        prefix,
        sizeof(prefix),
        VIPC_PROTOCOL_VERSION_STRING "|FRAME|%u|%lu|%lu|%lu|%lu|%lu|",
        (unsigned)message->kind,
        (unsigned long)message->flags,
        (unsigned long)message->operation,
        (unsigned long)low_u32(message->correlation_id),
        (unsigned long)high_u32(message->correlation_id),
        (unsigned long)payload_size);
    if (prefix_length < 0 || (size_t)prefix_length >= sizeof(prefix)) return NULL;

    encoded_length = ((size_t)payload_size + 2u) / 3u * 4u;
    total_length = (size_t)prefix_length + encoded_length;
    output = (char *)malloc(total_length + 1u);
    if (!output) return NULL;

    memcpy(output, prefix, (size_t)prefix_length);
    cursor = output + prefix_length;

    while (i + 3u <= payload_size) {
        uint32_t block = ((uint32_t)payload[i] << 16)
            | ((uint32_t)payload[i + 1u] << 8)
            | (uint32_t)payload[i + 2u];
        *cursor++ = alphabet[(block >> 18) & 63u];
        *cursor++ = alphabet[(block >> 12) & 63u];
        *cursor++ = alphabet[(block >> 6) & 63u];
        *cursor++ = alphabet[block & 63u];
        i += 3u;
    }

    if (i < payload_size) {
        uint32_t block = (uint32_t)payload[i] << 16;
        *cursor++ = alphabet[(block >> 18) & 63u];
        if (i + 1u < payload_size) {
            block |= (uint32_t)payload[i + 1u] << 8;
            *cursor++ = alphabet[(block >> 12) & 63u];
            *cursor++ = alphabet[(block >> 6) & 63u];
            *cursor++ = '=';
        } else {
            *cursor++ = alphabet[(block >> 12) & 63u];
            *cursor++ = '=';
            *cursor++ = '=';
        }
    }

    *cursor = '\0';
    return output;
}

static int is_utf8_continuation(uint8_t byte) {
    return byte >= 0x80u && byte <= 0xbfu;
}

static int valid_utf8_no_nul(const uint8_t *data, uint32_t size) {
    uint32_t i = 0u;

    if (size == 0u) return 1;
    if (!data) return 0;

    while (i < size) {
        uint8_t a = data[i];
        if (a == 0u) return 0;
        if (a <= 0x7fu) {
            i += 1u;
            continue;
        }

        if (a >= 0xc2u && a <= 0xdfu) {
            if (i + 1u >= size || !is_utf8_continuation(data[i + 1u])) {
                return 0;
            }
            i += 2u;
            continue;
        }

        if (a == 0xe0u) {
            if (i + 2u >= size
                || data[i + 1u] < 0xa0u
                || data[i + 1u] > 0xbfu
                || !is_utf8_continuation(data[i + 2u])) {
                return 0;
            }
            i += 3u;
            continue;
        }

        if ((a >= 0xe1u && a <= 0xecu)
            || (a >= 0xeeu && a <= 0xefu)) {
            if (i + 2u >= size
                || !is_utf8_continuation(data[i + 1u])
                || !is_utf8_continuation(data[i + 2u])) {
                return 0;
            }
            i += 3u;
            continue;
        }

        if (a == 0xedu) {
            if (i + 2u >= size
                || data[i + 1u] < 0x80u
                || data[i + 1u] > 0x9fu
                || !is_utf8_continuation(data[i + 2u])) {
                return 0;
            }
            i += 3u;
            continue;
        }

        if (a == 0xf0u) {
            if (i + 3u >= size
                || data[i + 1u] < 0x90u
                || data[i + 1u] > 0xbfu
                || !is_utf8_continuation(data[i + 2u])
                || !is_utf8_continuation(data[i + 3u])) {
                return 0;
            }
            i += 4u;
            continue;
        }

        if (a >= 0xf1u && a <= 0xf3u) {
            if (i + 3u >= size
                || !is_utf8_continuation(data[i + 1u])
                || !is_utf8_continuation(data[i + 2u])
                || !is_utf8_continuation(data[i + 3u])) {
                return 0;
            }
            i += 4u;
            continue;
        }

        if (a == 0xf4u) {
            if (i + 3u >= size
                || data[i + 1u] < 0x80u
                || data[i + 1u] > 0x8fu
                || !is_utf8_continuation(data[i + 2u])
                || !is_utf8_continuation(data[i + 3u])) {
                return 0;
            }
            i += 4u;
            continue;
        }

        return 0;
    }

    return 1;
}

static char *text_frame(
    const vipc_message *message,
    const uint8_t *payload,
    uint32_t payload_size) {
    char prefix[192];
    int prefix_length;
    size_t total_length;
    char *output;

    if (!message || !valid_utf8_no_nul(payload, payload_size)) {
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|DATA|TEXT_RESPONSE");
    }

    prefix_length = snprintf(
        prefix,
        sizeof(prefix),
        VIPC_PROTOCOL_VERSION_STRING "|TEXT|%u|%lu|%lu|%lu|%lu|%lu|",
        (unsigned)message->kind,
        (unsigned long)message->flags,
        (unsigned long)message->operation,
        (unsigned long)low_u32(message->correlation_id),
        (unsigned long)high_u32(message->correlation_id),
        (unsigned long)payload_size);
    if (prefix_length < 0 || (size_t)prefix_length >= sizeof(prefix)) return NULL;

    total_length = (size_t)prefix_length + (size_t)payload_size;
    output = (char *)malloc(total_length + 1u);
    if (!output) return NULL;

    memcpy(output, prefix, (size_t)prefix_length);
    if (payload_size != 0u) {
        memcpy(output + prefix_length, payload, payload_size);
    }
    output[total_length] = '\0';
    return output;
}

static uint32_t next_generation(uint32_t generation) {
    generation += 1u;
    if (generation == 0u || generation > VIPC_EO_GENERATION_MAX) {
        generation = 1u;
    }
    return generation;
}

static uint32_t session_handle(uint32_t slot, uint32_t generation) {
    return (generation << VIPC_EO_SLOT_BITS) | slot;
}

static void session_clear_resources(vipc_eo_session *session) {
    if (!session) return;

    if (session->channel) {
        vipc_channel_destroy(session->channel);
        session->channel = NULL;
    }

    free(session->stage);
    free(session->receive);
    session->stage = NULL;
    session->receive = NULL;
    session->stage_size = 0u;
    session->stage_capacity = 0u;
    session->receive_capacity = 0u;
}

static void session_release(vipc_eo_session *session) {
    if (!session) return;
    session_clear_resources(session);
    session->in_use = 0;
    session->generation = next_generation(session->generation);
}

static vipc_eo_session *session_lookup(uint32_t handle) {
    uint32_t slot = handle & VIPC_EO_SLOT_MASK;
    uint32_t generation = handle >> VIPC_EO_SLOT_BITS;
    vipc_eo_session *session;

    if (slot >= VIPC_EO_MAX_SESSIONS || generation == 0u) {
        return NULL;
    }

    session = &g_sessions[slot];

    if (!session->in_use || session->generation != generation) {
        return NULL;
    }
    return session;
}

static vipc_eo_session *session_allocate(uint32_t *out_handle) {
    uint32_t slot;

    if (!out_handle) return NULL;

    for (slot = 0u; slot < VIPC_EO_MAX_SESSIONS; ++slot) {
        vipc_eo_session *session = &g_sessions[slot];
        if (session->in_use) continue;

        if (session->generation == 0u) session->generation = 1u;
        session_clear_resources(session);
        session->in_use = 1;
        *out_handle = session_handle(slot, session->generation);
        return session;
    }

    return NULL;
}

static vipc_eo_session *session_from_arg(
    const vipc_es_tagged_data *arg,
    uint32_t *out_handle) {
    uint32_t handle;
    vipc_eo_session *session;

    if (!numeric_to_u32(arg, &handle)) return NULL;
    session = session_lookup(handle);
    if (!session) return NULL;

    if (out_handle) *out_handle = handle;
    return session;
}

static void release_if_dead(vipc_eo_session *session) {
    if (!session || !session->in_use || !session->channel) return;
    if (!vipc_channel_is_open(session->channel)) {
        session_release(session);
    }
}

static void clear_all_sessions(void) {
    uint32_t slot;
    for (slot = 0u; slot < VIPC_EO_MAX_SESSIONS; ++slot) {
        if (g_sessions[slot].in_use) {
            session_release(&g_sessions[slot]);
        } else {
            session_clear_resources(&g_sessions[slot]);
        }
    }
}

static char *command_info(void) {
    return format_simple(
        VIPC_PROTOCOL_VERSION_STRING "|INFO|%u|%u|%u|%lu|%u|%u",
        VIPC_EO_ADAPTER_VERSION,
        VIPC_WIRE_MAJOR,
        VIPC_WIRE_MINOR,
        (unsigned long)VIPC_EO_MAX_PAYLOAD_BYTES,
        VIPC_EO_PACK_BYTES,
        VIPC_EO_MAX_SESSIONS);
}

static char *command_connect(
    const vipc_es_tagged_data *argv,
    long argc) {
    uint32_t timeout_ms;
    uint32_t endpoint_size;
    uint32_t packed_count;
    uint32_t handle = 0u;
    char endpoint[VIPC_ENDPOINT_MAX + 1u];
    vipc_eo_session *session;
    vipc_error error;
    vipc_status status;
    char *result;

    if (argc < 4
        || !numeric_to_u32(&argv[1], &timeout_ms)
        || !numeric_to_u32(&argv[2], &endpoint_size)
        || endpoint_size == 0u
        || endpoint_size > VIPC_ENDPOINT_MAX) {
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|ARGS|CONNECT");
    }

    packed_count = packed_count_for_bytes(endpoint_size);
    if ((uint32_t)(argc - 3) != packed_count) {
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|ARGS|CONNECT");
    }
    if (!unpack_bytes(&argv[3], packed_count, (uint8_t *)endpoint, endpoint_size)) {
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|ARGS|CONNECT");
    }
    endpoint[endpoint_size] = '\0';
    if (!vipc_endpoint_is_valid(endpoint)) {
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|ARGS|ENDPOINT");
    }

    session = session_allocate(&handle);
    if (!session) {
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|STATE|SESSION_LIMIT");
    }

    status = vipc_client_connect(
        endpoint,
        timeout_ms,
        &session->channel,
        &error);
    if (status != VIPC_OK) {
        session_release(session);
        return format_error(status, &error);
    }

    result = format_simple(
        VIPC_PROTOCOL_VERSION_STRING "|OK|CONNECTED|%lu|%lu|%lu",
        (unsigned long)handle,
        (unsigned long)vipc_channel_peer_pid(session->channel),
        (unsigned long)vipc_channel_peer_session_id(session->channel));
    if (!result) {
        session_release(session);
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|MEMORY|RETURN");
    }
    return result;
}

static char *command_stage_reset(
    const vipc_es_tagged_data *argv,
    long argc) {
    vipc_eo_session *session;

    if (argc != 2) return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|ARGS|STAGE_RESET");
    session = session_from_arg(&argv[1], NULL);
    if (!session) return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|STATE|INVALID_HANDLE");

    session->stage_size = 0u;
    return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|OK|STAGE_RESET");
}

static char *command_stage_append(
    const vipc_es_tagged_data *argv,
    long argc) {
    vipc_eo_session *session;
    uint32_t byte_count;
    uint32_t packed_count;
    uint32_t required;

    if (argc < 3 || !numeric_to_u32(&argv[2], &byte_count)) {
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|ARGS|STAGE_APPEND");
    }

    session = session_from_arg(&argv[1], NULL);
    if (!session) return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|STATE|INVALID_HANDLE");

    if (byte_count > VIPC_EO_MAX_PAYLOAD_BYTES - session->stage_size) {
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|LIMIT|STAGE");
    }
    packed_count = packed_count_for_bytes(byte_count);
    if ((uint32_t)(argc - 3) != packed_count) {
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|ARGS|STAGE_APPEND");
    }

    required = session->stage_size + byte_count;
    if (!reserve_buffer(
            &session->stage,
            &session->stage_capacity,
            required)) {
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|MEMORY|STAGE");
    }
    if (!unpack_bytes(
            packed_count ? &argv[3] : NULL,
            packed_count,
            byte_count
                ? session->stage + session->stage_size
                : NULL,
            byte_count)) {
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|ARGS|STAGE_APPEND");
    }

    session->stage_size = required;
    return format_simple(
        VIPC_PROTOCOL_VERSION_STRING "|OK|STAGED|%lu",
        (unsigned long)session->stage_size);
}

static char *receive_frame(
    vipc_eo_session *session,
    uint32_t timeout_ms) {
    vipc_message message;
    vipc_error error;
    vipc_status status;
    uint32_t payload_size = 0u;
    char *result;

    if (!session || !session->channel || !vipc_channel_is_open(session->channel)) {
        if (session) session_release(session);
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|STATE|NOT_CONNECTED");
    }

    if (!reserve_buffer(
            &session->receive,
            &session->receive_capacity,
            VIPC_EO_MAX_PAYLOAD_BYTES)) {
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|MEMORY|RECEIVE");
    }

    status = vipc_channel_receive(
        session->channel,
        &message,
        session->receive,
        session->receive_capacity,
        &payload_size,
        timeout_ms,
        &error);
    if (status != VIPC_OK) {
        result = format_error(status, &error);
        release_if_dead(session);
        return result;
    }

    result = base64_frame(&message, session->receive, payload_size);
    return result ? result : duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|MEMORY|RETURN");
}

static char *transact_payload(
    vipc_eo_session *session,
    uint32_t timeout_ms,
    uint32_t operation,
    uint64_t correlation,
    const uint8_t *request_payload,
    uint32_t request_size,
    int text_response) {
    vipc_message message;
    vipc_error error;
    vipc_status status;
    vipc_message response;
    uint32_t payload_size = 0u;
    char *result;
#ifdef _WIN32
    ULONGLONG started;
#endif

    if (!session->channel || !vipc_channel_is_open(session->channel)) {
        session_release(session);
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|STATE|NOT_CONNECTED");
    }
    if (request_size > VIPC_EO_MAX_PAYLOAD_BYTES
        || (request_size != 0u && !request_payload)) {
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|LIMIT|REQUEST");
    }

    /*
     * Reserve before sending. If allocation failed after the request was on the
     * wire, its response could remain unread and desynchronize the stream.
     */
    if (!reserve_buffer(
            &session->receive,
            &session->receive_capacity,
            VIPC_EO_MAX_PAYLOAD_BYTES)) {
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|MEMORY|RECEIVE");
    }

    message.kind = VIPC_KIND_REQUEST;
    message.flags = VIPC_FLAG_NONE;
    message.operation = operation;
    message.correlation_id = correlation;
    message.payload_size = request_size;

#ifdef _WIN32
    started = GetTickCount64();
#endif
    status = vipc_channel_send(
        session->channel,
        &message,
        request_size ? request_payload : NULL,
        timeout_ms,
        &error);
    if (status != VIPC_OK) {
        result = format_error(status, &error);
        release_if_dead(session);
        return result;
    }

    status = vipc_channel_receive(
        session->channel,
        &response,
        session->receive,
        session->receive_capacity,
        &payload_size,
#ifdef _WIN32
        remaining_timeout_ms(started, timeout_ms),
#else
        timeout_ms,
#endif
        &error);
    if (status != VIPC_OK) {
        result = format_error(status, &error);
        release_if_dead(session);
        return result;
    }

    if (response.kind != VIPC_KIND_RESPONSE
        || response.operation != operation
        || response.correlation_id != correlation) {
        session_release(session);
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|PROTOCOL|CORRELATION");
    }

    result = text_response
        ? text_frame(&response, session->receive, payload_size)
        : base64_frame(&response, session->receive, payload_size);
    return result ? result : duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|MEMORY|RETURN");
}

static char *command_transact(
    const vipc_es_tagged_data *argv,
    long argc) {
    vipc_eo_session *session;
    uint32_t timeout_ms;
    uint32_t operation;
    uint32_t correlation_low;
    uint32_t correlation_high;

    if (argc != 6
        || !numeric_to_u32(&argv[2], &timeout_ms)
        || !numeric_to_u32(&argv[3], &operation)
        || !numeric_to_u32(&argv[4], &correlation_low)
        || !numeric_to_u32(&argv[5], &correlation_high)) {
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|ARGS|TRANSACT");
    }

    session = session_from_arg(&argv[1], NULL);
    if (!session) return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|STATE|INVALID_HANDLE");

    return transact_payload(
        session,
        timeout_ms,
        operation,
        join_u64(correlation_low, correlation_high),
        session->stage_size ? session->stage : NULL,
        session->stage_size,
        0);
}

static char *command_transact_text(
    const vipc_es_tagged_data *argv,
    long argc) {
    vipc_eo_session *session;
    uint32_t timeout_ms;
    uint32_t operation;
    uint32_t correlation_low;
    uint32_t correlation_high;
    const char *text;
    size_t text_size;

    if (argc != 7
        || !numeric_to_u32(&argv[2], &timeout_ms)
        || !numeric_to_u32(&argv[3], &operation)
        || !numeric_to_u32(&argv[4], &correlation_low)
        || !numeric_to_u32(&argv[5], &correlation_high)
        || argv[6].type != VIPC_ES_TYPE_STRING
        || !argv[6].data.string) {
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|ARGS|TRANSACT_TEXT");
    }

    session = session_from_arg(&argv[1], NULL);
    if (!session) return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|STATE|INVALID_HANDLE");

    text = argv[6].data.string;
    text_size = strlen(text);
    if (text_size > VIPC_EO_MAX_PAYLOAD_BYTES) {
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|LIMIT|TEXT_REQUEST");
    }
    if (!valid_utf8_no_nul((const uint8_t *)text, (uint32_t)text_size)) {
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|DATA|TEXT_REQUEST");
    }

    return transact_payload(
        session,
        timeout_ms,
        operation,
        join_u64(correlation_low, correlation_high),
        (const uint8_t *)text,
        (uint32_t)text_size,
        1);
}

static char *command_send(
    const vipc_es_tagged_data *argv,
    long argc) {
    vipc_eo_session *session;
    uint32_t kind;
    uint32_t flags;
    uint32_t operation;
    uint32_t correlation_low;
    uint32_t correlation_high;
    uint32_t timeout_ms;
    vipc_message message;
    vipc_error error;
    vipc_status status;
    char *result;

    if (argc != 8
        || !numeric_to_u32(&argv[2], &kind)
        || !numeric_to_u32(&argv[3], &flags)
        || !numeric_to_u32(&argv[4], &operation)
        || !numeric_to_u32(&argv[5], &correlation_low)
        || !numeric_to_u32(&argv[6], &correlation_high)
        || !numeric_to_u32(&argv[7], &timeout_ms)
        || kind > UINT16_MAX) {
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|ARGS|SEND");
    }

    session = session_from_arg(&argv[1], NULL);
    if (!session) return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|STATE|INVALID_HANDLE");
    if (!session->channel || !vipc_channel_is_open(session->channel)) {
        session_release(session);
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|STATE|NOT_CONNECTED");
    }

    message.kind = kind;
    message.flags = flags;
    message.operation = operation;
    message.correlation_id = join_u64(correlation_low, correlation_high);
    message.payload_size = session->stage_size;

    status = vipc_channel_send(
        session->channel,
        &message,
        session->stage_size ? session->stage : NULL,
        timeout_ms,
        &error);
    if (status != VIPC_OK) {
        result = format_error(status, &error);
        release_if_dead(session);
        return result;
    }

    return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|OK|SENT");
}

static char *command_receive(
    const vipc_es_tagged_data *argv,
    long argc) {
    vipc_eo_session *session;
    uint32_t timeout_ms;

    if (argc != 3 || !numeric_to_u32(&argv[2], &timeout_ms)) {
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|ARGS|RECEIVE");
    }

    session = session_from_arg(&argv[1], NULL);
    if (!session) return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|STATE|INVALID_HANDLE");
    return receive_frame(session, timeout_ms);
}

static char *command_close(
    const vipc_es_tagged_data *argv,
    long argc) {
    vipc_eo_session *session;

    if (argc != 2) return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|ARGS|CLOSE");
    session = session_from_arg(&argv[1], NULL);
    if (!session) return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|STATE|INVALID_HANDLE");

    session_release(session);
    return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|OK|CLOSED");
}

static char *dispatch(
    vipc_es_tagged_data *argv,
    long argc) {
    uint32_t command;

    if (!argv || argc <= 0 || !numeric_to_u32(&argv[0], &command)) {
        return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|ARGS|COMMAND");
    }

    switch (command) {
        case VIPC_EO_CMD_INFO:
            return argc == 1
                ? command_info()
                : duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|ARGS|INFO");
        case VIPC_EO_CMD_CONNECT:
            return command_connect(argv, argc);
        case VIPC_EO_CMD_STAGE_RESET:
            return command_stage_reset(argv, argc);
        case VIPC_EO_CMD_STAGE_APPEND:
            return command_stage_append(argv, argc);
        case VIPC_EO_CMD_TRANSACT:
            return command_transact(argv, argc);
        case VIPC_EO_CMD_SEND:
            return command_send(argv, argc);
        case VIPC_EO_CMD_RECEIVE:
            return command_receive(argv, argc);
        case VIPC_EO_CMD_CLOSE:
            return command_close(argv, argc);
        case VIPC_EO_CMD_TRANSACT_TEXT:
            return command_transact_text(argv, argc);
        default:
            return duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|ARGS|UNKNOWN_COMMAND");
    }
}

long vipc(
    vipc_es_tagged_data *argv,
    long argc,
    vipc_es_tagged_data *retval) {
    char *result;

    set_undefined(retval);
    if (!retval) return VIPC_ES_ERR_OK;

#ifdef _WIN32
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) {
        result = duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|STATE|BUSY");
        (void)set_string_owned(retval, result);
        return VIPC_ES_ERR_OK;
    }
#endif

    result = dispatch(argv, argc);
    if (!result) result = duplicate_string(VIPC_PROTOCOL_VERSION_STRING "|ERR|MEMORY|RETURN");
    (void)set_string_owned(retval, result);

#ifdef _WIN32
    (void)InterlockedExchange(&g_busy, 0);
#endif
    return VIPC_ES_ERR_OK;
}

char *ESInitialize(
    const vipc_es_tagged_data **argv,
    long argc) {
    (void)argv;
    (void)argc;
    /*
     * Keep one critical method, first and only. No signature suffix means JS
     * numeric arguments arrive through the empirically reliable default lane.
     */
    return "vipc";
}

long ESGetVersion(void) {
    return (long)VIPC_EO_ADAPTER_VERSION;
}

void ESFreeMem(void *pointer) {
    free(pointer);
}

void ESTerminate(void) {
    clear_all_sessions();
}
