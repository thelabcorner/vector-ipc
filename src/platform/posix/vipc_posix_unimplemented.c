#include "vectoripc/vipc.h"

#include <stdlib.h>

/*
 * The POSIX/macOS transport is intentionally not claimed as implemented yet.
 * This file keeps the public library buildable on non-Windows hosts while the
 * Unix-domain-socket transport is specified and tested on a real macOS target.
 */

struct vipc_server { int unused; };
struct vipc_channel { int unused; };

static vipc_status unsupported(vipc_error *error, vipc_phase phase) {
    if (error) {
        error->status = VIPC_ERR_UNSUPPORTED;
        error->phase = phase;
        error->platform_code = 0;
    }
    return VIPC_ERR_UNSUPPORTED;
}

vipc_status vipc_server_create(
    const char *endpoint,
    vipc_server **out_server,
    vipc_error *error) {
    (void)endpoint;
    if (out_server) *out_server = NULL;
    return unsupported(error, VIPC_PHASE_LISTEN);
}

void vipc_server_destroy(vipc_server *server) {
    free(server);
}

vipc_status vipc_server_accept(
    vipc_server *server,
    uint32_t timeout_ms,
    vipc_channel **out_channel,
    vipc_error *error) {
    (void)server;
    (void)timeout_ms;
    if (out_channel) *out_channel = NULL;
    return unsupported(error, VIPC_PHASE_ACCEPT);
}

vipc_status vipc_client_connect(
    const char *endpoint,
    uint32_t timeout_ms,
    vipc_channel **out_channel,
    vipc_error *error) {
    (void)endpoint;
    (void)timeout_ms;
    if (out_channel) *out_channel = NULL;
    return unsupported(error, VIPC_PHASE_CONNECT);
}

void vipc_channel_destroy(vipc_channel *channel) {
    free(channel);
}

int vipc_channel_is_open(const vipc_channel *channel) {
    (void)channel;
    return 0;
}

uint32_t vipc_channel_peer_pid(const vipc_channel *channel) {
    (void)channel;
    return 0;
}

uint32_t vipc_channel_peer_session_id(const vipc_channel *channel) {
    (void)channel;
    return 0;
}

vipc_status vipc_channel_send(
    vipc_channel *channel,
    const vipc_message *message,
    const void *payload,
    uint32_t timeout_ms,
    vipc_error *error) {
    (void)channel;
    (void)message;
    (void)payload;
    (void)timeout_ms;
    return unsupported(error, VIPC_PHASE_WRITE_HEADER);
}

vipc_status vipc_channel_receive(
    vipc_channel *channel,
    vipc_message *out_message,
    void *payload_buffer,
    uint32_t payload_capacity,
    uint32_t *out_payload_size,
    uint32_t timeout_ms,
    vipc_error *error) {
    (void)channel;
    (void)out_message;
    (void)payload_buffer;
    (void)payload_capacity;
    (void)timeout_ms;
    if (out_payload_size) *out_payload_size = 0;
    return unsupported(error, VIPC_PHASE_READ_HEADER);
}