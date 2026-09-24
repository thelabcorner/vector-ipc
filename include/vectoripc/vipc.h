#ifndef VECTORIPC_VIPC_H
#define VECTORIPC_VIPC_H

#include <stddef.h>
#include <stdint.h>

#include "vipc_protocol.h"

#if defined(_WIN32) && defined(VIPC_BUILD_DLL)
#define VIPC_API __declspec(dllexport)
#elif defined(_WIN32) && defined(VIPC_USE_DLL)
#define VIPC_API __declspec(dllimport)
#else
#define VIPC_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define VIPC_VERSION_MAJOR 0u
#define VIPC_VERSION_MINOR 1u
#define VIPC_VERSION_PATCH 0u
#define VIPC_VERSION_STRING "0.1.0"

#define VIPC_ABI_VERSION 1u
#define VIPC_ENDPOINT_MAX 80u
#define VIPC_DEFAULT_PIPE_BUFFER_BYTES (64u * 1024u)
#define VIPC_CANCEL_SETTLE_MS 25u
#define VIPC_TIMEOUT_MAX_MS (24u * 60u * 60u * 1000u)

typedef uint32_t vipc_status;
enum {
    VIPC_OK = 0u,
    VIPC_ERR_INVALID_ARGUMENT = 1u,
    VIPC_ERR_INVALID_ENDPOINT = 2u,
    VIPC_ERR_OUT_OF_MEMORY = 3u,
    VIPC_ERR_UNSUPPORTED = 4u,
    VIPC_ERR_NOT_CONNECTED = 5u,
    VIPC_ERR_BUSY = 6u,
    VIPC_ERR_TIMEOUT = 7u,
    VIPC_ERR_CANCEL_STUCK = 8u,
    VIPC_ERR_ACCESS_DENIED = 9u,
    VIPC_ERR_ENDPOINT_NOT_FOUND = 10u,
    VIPC_ERR_ENDPOINT_BUSY = 11u,
    VIPC_ERR_ENDPOINT_IN_USE = 12u,
    VIPC_ERR_PEER_CLOSED = 13u,
    VIPC_ERR_IO = 14u,
    VIPC_ERR_PROTOCOL_MAGIC = 15u,
    VIPC_ERR_PROTOCOL_VERSION = 16u,
    VIPC_ERR_PROTOCOL_INVALID = 17u,
    VIPC_ERR_PAYLOAD_TOO_LARGE = 18u,
    VIPC_ERR_BUFFER_TOO_SMALL = 19u,
    VIPC_ERR_INTERNAL = 20u
};

typedef uint32_t vipc_phase;
enum {
    VIPC_PHASE_NONE = 0u,
    VIPC_PHASE_ENDPOINT = 1u,
    VIPC_PHASE_SECURITY = 2u,
    VIPC_PHASE_LISTEN = 3u,
    VIPC_PHASE_ACCEPT = 4u,
    VIPC_PHASE_CONNECT = 5u,
    VIPC_PHASE_WRITE_HEADER = 6u,
    VIPC_PHASE_WRITE_PAYLOAD = 7u,
    VIPC_PHASE_READ_HEADER = 8u,
    VIPC_PHASE_READ_PAYLOAD = 9u,
    VIPC_PHASE_CANCEL = 10u
};

typedef struct vipc_error {
    vipc_status status;
    vipc_phase phase;
    uint32_t platform_code;
} vipc_error;

#if defined(__cplusplus)
static_assert(sizeof(vipc_status) == 4u, "vipc_status ABI width");
static_assert(sizeof(vipc_phase) == 4u, "vipc_phase ABI width");
static_assert(sizeof(vipc_error) == 12u, "vipc_error ABI size");
static_assert(offsetof(vipc_error, status) == 0u, "vipc_error.status ABI offset");
static_assert(offsetof(vipc_error, phase) == 4u, "vipc_error.phase ABI offset");
static_assert(offsetof(vipc_error, platform_code) == 8u, "vipc_error.platform_code ABI offset");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(vipc_status) == 4u, "vipc_status ABI width");
_Static_assert(sizeof(vipc_phase) == 4u, "vipc_phase ABI width");
_Static_assert(sizeof(vipc_error) == 12u, "vipc_error ABI size");
_Static_assert(offsetof(vipc_error, status) == 0u, "vipc_error.status ABI offset");
_Static_assert(offsetof(vipc_error, phase) == 4u, "vipc_error.phase ABI offset");
_Static_assert(offsetof(vipc_error, platform_code) == 8u, "vipc_error.platform_code ABI offset");
#endif

typedef struct vipc_server vipc_server;
typedef struct vipc_channel vipc_channel;

VIPC_API uint32_t vipc_abi_version(void);
VIPC_API const char *vipc_status_name(vipc_status status);
VIPC_API const char *vipc_phase_name(vipc_phase phase);
VIPC_API void vipc_error_clear(vipc_error *error);

/* Endpoint tokens are product names, not OS paths. */
VIPC_API int vipc_endpoint_is_valid(const char *endpoint);

/*
 * Server lifecycle.
 *
 * vipc_server_accept() is single-acceptor. Accepted channels may be handed to
 * other threads and used concurrently with the accept loop.
 *
 * vipc_server_destroy() must not race an in-flight vipc_server_accept(). Use a
 * finite accept timeout to observe product shutdown, let accept return, then
 * destroy the server.
 */
VIPC_API vipc_status vipc_server_create(
    const char *endpoint,
    vipc_server **out_server,
    vipc_error *error);

VIPC_API void vipc_server_destroy(vipc_server *server);

VIPC_API vipc_status vipc_server_accept(
    vipc_server *server,
    uint32_t timeout_ms,
    vipc_channel **out_channel,
    vipc_error *error);

/* Client connection. */
VIPC_API vipc_status vipc_client_connect(
    const char *endpoint,
    uint32_t timeout_ms,
    vipc_channel **out_channel,
    vipc_error *error);

/*
 * Channel lifecycle and framed I/O.
 *
 * One send and one receive may run concurrently. Two sends or two receives on
 * the same channel return VIPC_ERR_BUSY.
 *
 * vipc_channel_destroy() must not race an in-flight send/receive. Finite
 * operation timeouts are the shutdown boundary: let active calls return/join
 * before destroying the channel.
 */
VIPC_API void vipc_channel_destroy(vipc_channel *channel);
VIPC_API int vipc_channel_is_open(const vipc_channel *channel);
VIPC_API uint32_t vipc_channel_peer_pid(const vipc_channel *channel);
VIPC_API uint32_t vipc_channel_peer_session_id(const vipc_channel *channel);

VIPC_API vipc_status vipc_channel_send(
    vipc_channel *channel,
    const vipc_message *message,
    const void *payload,
    uint32_t timeout_ms,
    vipc_error *error);

/*
 * On success, out_message describes the received frame and *out_payload_size is
 * the payload bytes copied.
 *
 * If a valid payload is larger than payload_capacity, VectorIPC drains the frame
 * to preserve synchronization, sets out_message->payload_size to the required
 * size, sets *out_payload_size to 0, and returns VIPC_ERR_BUFFER_TOO_SMALL.
 */
VIPC_API vipc_status vipc_channel_receive(
    vipc_channel *channel,
    vipc_message *out_message,
    void *payload_buffer,
    uint32_t payload_capacity,
    uint32_t *out_payload_size,
    uint32_t timeout_ms,
    vipc_error *error);

#ifdef __cplusplus
}
#endif

#endif
