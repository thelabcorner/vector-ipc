#include "vectoripc/vipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define DEFAULT_ACCEPT_CYCLES 250u
#define DEFAULT_WRITE_CYCLES 50u
#define WRITE_TIMEOUT_MS 1u
#define WRITE_PAYLOAD_BYTES VIPC_MAX_PAYLOAD_BYTES

typedef struct silent_context {
    vipc_server *server;
    HANDLE accepted;
    HANDLE release_peer;
    HANDLE released;
    uint32_t cycles;
    int failed;
} silent_context;

typedef struct client_once_context {
    const char *endpoint;
    HANDLE connected;
    vipc_status status;
    vipc_error error;
} client_once_context;

static int parse_u32(const char *text, uint32_t *out_value) {
    char *end = NULL;
    unsigned long value;
    if (!text || !out_value || !*text) return 0;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value == 0u || value > UINT32_MAX) {
        return 0;
    }
    *out_value = (uint32_t)value;
    return 1;
}

static DWORD WINAPI silent_server_main(LPVOID opaque) {
    silent_context *context = (silent_context *)opaque;
    uint32_t i;

    for (i = 0u; i < context->cycles; ++i) {
        vipc_channel *channel = NULL;
        vipc_error error;
        vipc_status status = vipc_server_accept(
            context->server,
            5000u,
            &channel,
            &error);
        if (status != VIPC_OK || !channel) {
            context->failed = 1;
            return 1u;
        }

        (void)SetEvent(context->accepted);
        if (WaitForSingleObject(context->release_peer, 5000u) != WAIT_OBJECT_0) {
            context->failed = 1;
            vipc_channel_destroy(channel);
            return 1u;
        }

        vipc_channel_destroy(channel);
        (void)SetEvent(context->released);
    }

    return 0u;
}

static int test_write_timeouts(uint32_t cycles) {
    char endpoint[64];
    vipc_server *server = NULL;
    vipc_error error;
    vipc_status status;
    silent_context context;
    HANDLE thread = NULL;
    DWORD thread_id = 0u;
    DWORD handles_before = 0u;
    DWORD handles_after = 0u;
    DWORD wait_result;
    uint8_t *payload = NULL;
    uint32_t i;
    int failed = 0;

    ZeroMemory(&context, sizeof(context));
    if (!GetProcessHandleCount(GetCurrentProcess(), &handles_before)) return 0;

    payload = (uint8_t *)malloc(WRITE_PAYLOAD_BYTES);
    if (!payload) return 0;
    for (i = 0u; i < WRITE_PAYLOAD_BYTES; ++i) {
        payload[i] = (uint8_t)(i * 29u + 7u);
    }

    (void)sprintf_s(
        endpoint,
        sizeof(endpoint),
        "write-timeout-%lu",
        (unsigned long)GetCurrentProcessId());

    status = vipc_server_create(endpoint, &server, &error);
    if (status != VIPC_OK) {
        failed = 1;
        goto cleanup;
    }

    context.server = server;
    context.cycles = cycles;
    context.accepted = CreateEventW(NULL, FALSE, FALSE, NULL);
    context.release_peer = CreateEventW(NULL, FALSE, FALSE, NULL);
    context.released = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!context.accepted || !context.release_peer || !context.released) {
        failed = 1;
        goto cleanup;
    }

    thread = CreateThread(NULL, 0, silent_server_main, &context, 0, &thread_id);
    if (!thread) {
        failed = 1;
        goto cleanup;
    }

    for (i = 0u; i < cycles; ++i) {
        vipc_channel *client = NULL;
        vipc_message message;

        status = vipc_client_connect(endpoint, 5000u, &client, &error);
        if (status != VIPC_OK || !client) {
            fprintf(stderr, "write-timeout connect failed at %lu: %s\n",
                (unsigned long)i, vipc_status_name(status));
            failed = 1;
            break;
        }

        if (WaitForSingleObject(context.accepted, 5000u) != WAIT_OBJECT_0) {
            fprintf(stderr, "write-timeout accept acknowledgment failed at %lu\n",
                (unsigned long)i);
            vipc_channel_destroy(client);
            failed = 1;
            break;
        }

        message.kind = VIPC_KIND_NOTIFY;
        message.flags = VIPC_FLAG_NONE;
        message.operation = VIPC_APP_OPERATION_MIN + 120u;
        message.correlation_id = (uint64_t)i + 1u;
        message.payload_size = WRITE_PAYLOAD_BYTES;

        status = vipc_channel_send(
            client,
            &message,
            payload,
            WRITE_TIMEOUT_MS,
            &error);
        if (status != VIPC_ERR_TIMEOUT || vipc_channel_is_open(client)) {
            fprintf(
                stderr,
                "blocked write did not time out cleanly at %lu: %s open=%d phase=%s winerr=%lu\n",
                (unsigned long)i,
                vipc_status_name(status),
                vipc_channel_is_open(client),
                vipc_phase_name(error.phase),
                (unsigned long)error.platform_code);
            failed = 1;
        }

        (void)SetEvent(context.release_peer);
        vipc_channel_destroy(client);

        if (WaitForSingleObject(context.released, 5000u) != WAIT_OBJECT_0) {
            fprintf(stderr, "write-timeout server release failed at %lu\n",
                (unsigned long)i);
            failed = 1;
        }
        if (failed) break;
    }

    if (failed) {
        while (i++ < cycles) (void)SetEvent(context.release_peer);
    }

cleanup:
    if (thread) {
        wait_result = WaitForSingleObject(thread, 10000u);
        if (wait_result != WAIT_OBJECT_0) failed = 1;
        (void)CloseHandle(thread);
    }
    if (context.accepted) (void)CloseHandle(context.accepted);
    if (context.release_peer) (void)CloseHandle(context.release_peer);
    if (context.released) (void)CloseHandle(context.released);
    if (context.failed) failed = 1;
    if (server) vipc_server_destroy(server);
    free(payload);

    if (!GetProcessHandleCount(GetCurrentProcess(), &handles_after)) return 0;
    if (handles_after > handles_before + 1u) {
        fprintf(stderr, "write-timeout handle growth: %lu -> %lu\n",
            (unsigned long)handles_before,
            (unsigned long)handles_after);
        failed = 1;
    }

    if (!failed) {
        printf(
            "VectorIPC blocked-write cancellation: PASS (%lu cycles, handles %lu -> %lu)\n",
            (unsigned long)cycles,
            (unsigned long)handles_before,
            (unsigned long)handles_after);
    }
    return !failed;
}

static DWORD WINAPI client_once_main(LPVOID opaque) {
    client_once_context *context = (client_once_context *)opaque;
    vipc_channel *channel = NULL;

    context->status = vipc_client_connect(
        context->endpoint,
        5000u,
        &channel,
        &context->error);
    if (context->status == VIPC_OK && channel) {
        (void)SetEvent(context->connected);
        Sleep(25u);
        vipc_channel_destroy(channel);
        return 0u;
    }
    (void)SetEvent(context->connected);
    return 1u;
}

static int test_accept_timeouts(uint32_t cycles) {
    char endpoint[64];
    vipc_server *server = NULL;
    vipc_error error;
    vipc_status status;
    DWORD handles_before = 0u;
    DWORD handles_after = 0u;
    uint32_t i;
    int failed = 0;

    if (!GetProcessHandleCount(GetCurrentProcess(), &handles_before)) return 0;

    (void)sprintf_s(
        endpoint,
        sizeof(endpoint),
        "accept-timeout-%lu",
        (unsigned long)GetCurrentProcessId());

    status = vipc_server_create(endpoint, &server, &error);
    if (status != VIPC_OK) return 0;

    for (i = 0u; i < cycles; ++i) {
        vipc_channel *unexpected = NULL;
        status = vipc_server_accept(server, 0u, &unexpected, &error);
        if (status != VIPC_ERR_TIMEOUT || unexpected != NULL) {
            fprintf(
                stderr,
                "accept timeout failed at %lu: %s channel=%p phase=%s winerr=%lu\n",
                (unsigned long)i,
                vipc_status_name(status),
                (void *)unexpected,
                vipc_phase_name(error.phase),
                (unsigned long)error.platform_code);
            if (unexpected) vipc_channel_destroy(unexpected);
            failed = 1;
            break;
        }
    }

    if (!failed) {
        client_once_context context;
        HANDLE thread;
        DWORD thread_id = 0u;
        vipc_channel *accepted = NULL;

        ZeroMemory(&context, sizeof(context));
        context.endpoint = endpoint;
        context.connected = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!context.connected) {
            failed = 1;
        } else {
            thread = CreateThread(NULL, 0, client_once_main, &context, 0, &thread_id);
            if (!thread) {
                failed = 1;
            } else {
                status = vipc_server_accept(server, 5000u, &accepted, &error);
                if (status != VIPC_OK || !accepted) {
                    fprintf(stderr, "listener recovery accept failed: %s\n",
                        vipc_status_name(status));
                    failed = 1;
                }
                if (WaitForSingleObject(context.connected, 5000u) != WAIT_OBJECT_0
                    || context.status != VIPC_OK) {
                    fprintf(stderr, "listener recovery client failed: %s\n",
                        vipc_status_name(context.status));
                    failed = 1;
                }
                if (accepted) vipc_channel_destroy(accepted);
                if (WaitForSingleObject(thread, 5000u) != WAIT_OBJECT_0) failed = 1;
                (void)CloseHandle(thread);
            }
            (void)CloseHandle(context.connected);
        }
    }

    vipc_server_destroy(server);

    if (!GetProcessHandleCount(GetCurrentProcess(), &handles_after)) return 0;
    if (handles_after > handles_before + 1u) {
        fprintf(stderr, "accept-timeout handle growth: %lu -> %lu\n",
            (unsigned long)handles_before,
            (unsigned long)handles_after);
        failed = 1;
    }

    if (!failed) {
        printf(
            "VectorIPC accept cancellation/recovery: PASS (%lu cycles, handles %lu -> %lu)\n",
            (unsigned long)cycles,
            (unsigned long)handles_before,
            (unsigned long)handles_after);
    }
    return !failed;
}

int main(int argc, char **argv) {
    uint32_t accept_cycles = DEFAULT_ACCEPT_CYCLES;
    uint32_t write_cycles = DEFAULT_WRITE_CYCLES;

    if (argc >= 2 && !parse_u32(argv[1], &accept_cycles)) return 2;
    if (argc >= 3 && !parse_u32(argv[2], &write_cycles)) return 2;
    if (argc > 3) {
        fprintf(stderr,
            "usage: vectoripc_cancellation_stress [accept-cycles] [write-cycles]\n");
        return 2;
    }

    if (!test_accept_timeouts(accept_cycles)) return 1;
    if (!test_write_timeouts(write_cycles)) return 1;
    return 0;
}

#else

int main(void) {
    printf("VectorIPC cancellation stress: SKIP (Windows only)\n");
    return 0;
}

#endif
