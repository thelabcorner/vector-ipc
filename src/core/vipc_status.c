#include "vectoripc/vipc.h"

uint32_t vipc_abi_version(void) {
    return VIPC_ABI_VERSION;
}

const char *vipc_status_name(vipc_status status) {
    switch (status) {
        case VIPC_OK: return "ok";
        case VIPC_ERR_INVALID_ARGUMENT: return "invalid-argument";
        case VIPC_ERR_INVALID_ENDPOINT: return "invalid-endpoint";
        case VIPC_ERR_OUT_OF_MEMORY: return "out-of-memory";
        case VIPC_ERR_UNSUPPORTED: return "unsupported";
        case VIPC_ERR_NOT_CONNECTED: return "not-connected";
        case VIPC_ERR_BUSY: return "busy";
        case VIPC_ERR_TIMEOUT: return "timeout";
        case VIPC_ERR_CANCEL_STUCK: return "cancel-stuck";
        case VIPC_ERR_ACCESS_DENIED: return "access-denied";
        case VIPC_ERR_ENDPOINT_NOT_FOUND: return "endpoint-not-found";
        case VIPC_ERR_ENDPOINT_BUSY: return "endpoint-busy";
        case VIPC_ERR_ENDPOINT_IN_USE: return "endpoint-in-use";
        case VIPC_ERR_PEER_CLOSED: return "peer-closed";
        case VIPC_ERR_IO: return "io";
        case VIPC_ERR_PROTOCOL_MAGIC: return "protocol-magic";
        case VIPC_ERR_PROTOCOL_VERSION: return "protocol-version";
        case VIPC_ERR_PROTOCOL_INVALID: return "protocol-invalid";
        case VIPC_ERR_PAYLOAD_TOO_LARGE: return "payload-too-large";
        case VIPC_ERR_BUFFER_TOO_SMALL: return "buffer-too-small";
        case VIPC_ERR_INTERNAL: return "internal";
        default: return "unknown";
    }
}

const char *vipc_phase_name(vipc_phase phase) {
    switch (phase) {
        case VIPC_PHASE_NONE: return "none";
        case VIPC_PHASE_ENDPOINT: return "endpoint";
        case VIPC_PHASE_SECURITY: return "security";
        case VIPC_PHASE_LISTEN: return "listen";
        case VIPC_PHASE_ACCEPT: return "accept";
        case VIPC_PHASE_CONNECT: return "connect";
        case VIPC_PHASE_WRITE_HEADER: return "write-header";
        case VIPC_PHASE_WRITE_PAYLOAD: return "write-payload";
        case VIPC_PHASE_READ_HEADER: return "read-header";
        case VIPC_PHASE_READ_PAYLOAD: return "read-payload";
        case VIPC_PHASE_CANCEL: return "cancel";
        default: return "unknown";
    }
}

void vipc_error_clear(vipc_error *error) {
    if (!error) return;
    error->status = VIPC_OK;
    error->phase = VIPC_PHASE_NONE;
    error->platform_code = 0;
}

int vipc_endpoint_is_valid(const char *endpoint) {
    size_t length = 0;
    const unsigned char *p;

    if (!endpoint || endpoint[0] == '\0') return 0;

    p = (const unsigned char *)endpoint;
    while (*p) {
        unsigned char c = *p++;
        ++length;
        if (length > VIPC_ENDPOINT_MAX) return 0;

        if ((c >= (unsigned char)'a' && c <= (unsigned char)'z')
            || (c >= (unsigned char)'A' && c <= (unsigned char)'Z')
            || (c >= (unsigned char)'0' && c <= (unsigned char)'9')
            || c == (unsigned char)'.'
            || c == (unsigned char)'_'
            || c == (unsigned char)'-') {
            continue;
        }
        return 0;
    }

    return length != 0;
}