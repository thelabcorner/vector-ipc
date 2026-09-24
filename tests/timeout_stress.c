#include "vectoripc/vipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define DEFAULT_CYCLES 250u

typedef struct timeout_context {
    vipc_server *server;
    HANDLE peer_accepted;
    HANDLE release_peer;
    HANDLE peer_released;
    uint32_t cycles;
    int failed;
} timeout_context;

static int parse_u32(const char *text, uint32_t *out_value) {
    char *end = NULL;
    unsigned long value;
    if (!text || !out_value || !*text) return 0;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value > UINT32_MAX || value == 0u) {
        return 0;
    }
    *out_value = (uint32_t)value;
    return 1;
}

static DWORD WINAPI silent_server(LPVOID opaque) {
    timeout_context *context = (timeout_context *)opaque;
    uint32_t i;

    for (i = 0u; i < context->cycles; ++i) {
        vipc_channel *channel = NULL;
        vipc_error error;
        vipc_status status = vipc_server_accept(
            context->server,
            5000u,
            &channel,
            &error);
        if (status != VIPC_OK) {
            context->failed = 1;
            return 1u;
        }

        (void)SetEvent(context->peer_accepted);

        if (WaitForSingleObject(context->release_peer, 5000u) != WAIT_OBJECT_0) {
            context->failed = 1;
            vipc_channel_destroy(channel);
            return 1u;
        }
        vipc_channel_destroy(channel);
        (void)SetEvent(context->peer_released);
    }
    return 0u;
}

int main(int argc, char **argv) {
    uint32_t cycles = DEFAULT_CYCLES;
    char endpoint[64];
    vipc_server *server = NULL;
    vipc_error error;
    vipc_status status;
    timeout_context context;
    HANDLE server_thread = NULL;
    DWORD thread_id = 0u;
    DWORD wait_result;
    DWORD handles_before = 0u;
    DWORD handles_after = 0u;
    uint32_t i;
    int failed = 0;

    if (argc == 2) {
        if (!parse_u32(argv[1], &cycles)) return 2;
    } else if (argc != 1) {
        fprintf(stderr, "usage: vectoripc_timeout_stress [cycles]\n");
        return 2;
    }

    ZeroMemory(&context, sizeof(context));
    if (!GetProcessHandleCount(GetCurrentProcess(), &handles_before)) return 2;

    (void)sprintf_s(
        endpoint,
        sizeof(endpoint),
        "timeout-%lu",
        (unsigned long)GetCurrentProcessId());

    status = vipc_server_create(endpoint, &server, &error);
    if (status != VIPC_OK) return 1;

    context.server = server;
    context.cycles = cycles;
    context.peer_accepted = CreateEventW(NULL, FALSE, FALSE, NULL);
    context.release_peer = CreateEventW(NULL, FALSE, FALSE, NULL);
    context.peer_released = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!context.peer_accepted
        || !context.release_peer
        || !context.peer_released) {
        if (context.peer_accepted) (void)CloseHandle(context.peer_accepted);
        if (context.release_peer) (void)CloseHandle(context.release_peer);
        if (context.peer_released) (void)CloseHandle(context.peer_released);
        vipc_server_destroy(server);
        return 1;
    }

    server_thread = CreateThread(NULL, 0, silent_server, &context, 0, &thread_id);
    if (!server_thread) {
        (void)CloseHandle(context.release_peer);
        vipc_server_destroy(server);
        return 1;
    }

    for (i = 0u; i < cycles; ++i) {
        vipc_channel *client = NULL;
        vipc_message message;
        uint8_t byte = 0u;
        uint32_t received = 0u;

        status = vipc_client_connect(endpoint, 5000u, &client, &error);
        if (status != VIPC_OK) {
            fprintf(stderr, "connect failed at %lu: %s\n",
                (unsigned long)i, vipc_status_name(status));
            failed = 1;
            break;
        }

        if (WaitForSingleObject(context.peer_accepted, 5000u) != WAIT_OBJECT_0) {
            fprintf(stderr, "server accept acknowledgment failed at %lu\n",
                (unsigned long)i);
            failed = 1;
            vipc_channel_destroy(client);
            break;
        }

        status = vipc_channel_receive(
            client,
            &message,
            &byte,
            1u,
            &received,
            0u,
            &error);
        if (status != VIPC_ERR_TIMEOUT || vipc_channel_is_open(client)) {
            fprintf(stderr, "timeout failed at %lu: %s open=%d\n",
                (unsigned long)i,
                vipc_status_name(status),
                vipc_channel_is_open(client));
            failed = 1;
        }

        (void)SetEvent(context.release_peer);
        vipc_channel_destroy(client);
        if (WaitForSingleObject(context.peer_released, 5000u) != WAIT_OBJECT_0) {
            fprintf(stderr, "server teardown acknowledgment failed at %lu\n",
                (unsigned long)i);
            failed = 1;
        }
        if (failed) break;
    }

    if (failed) {
        while (i++ < cycles) (void)SetEvent(context.release_peer);
    }

    wait_result = WaitForSingleObject(server_thread, 10000u);
    if (wait_result != WAIT_OBJECT_0) failed = 1;
    (void)CloseHandle(server_thread);
    (void)CloseHandle(context.peer_accepted);
    (void)CloseHandle(context.release_peer);
    (void)CloseHandle(context.peer_released);
    if (context.failed) failed = 1;
    vipc_server_destroy(server);

    if (!GetProcessHandleCount(GetCurrentProcess(), &handles_after)) return 2;
    if (handles_after > handles_before + 1u) {
        fprintf(stderr, "handle growth: before=%lu after=%lu\n",
            (unsigned long)handles_before,
            (unsigned long)handles_after);
        failed = 1;
    }

    if (failed) return 1;
    printf(
        "VectorIPC timeout/cancellation stress: PASS (%lu cycles, handles %lu -> %lu)\n",
        (unsigned long)cycles,
        (unsigned long)handles_before,
        (unsigned long)handles_after);
    return 0;
}

#else
int main(void) {
    printf("VectorIPC timeout stress: SKIP (Windows only)\n");
    return 0;
}
#endif
