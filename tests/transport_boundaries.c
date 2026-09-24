#include "vectoripc/vipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

static const uint32_t k_sizes[] = {
    0u, 1u, 31u, 32u, 33u,
    255u, 256u, 257u,
    4095u, 4096u, 4097u,
    65535u, 65536u, 65537u,
    262143u, 262144u, 262145u,
    1048576u,
    VIPC_MAX_PAYLOAD_BYTES
};

#define SIZE_COUNT ((uint32_t)(sizeof(k_sizes) / sizeof(k_sizes[0])))

typedef struct boundary_context {
    vipc_server *server;
    int failed;
} boundary_context;

static uint8_t pattern_byte(uint32_t size, uint32_t index) {
    uint32_t x = size * 2654435761u;
    x ^= index * 2246822519u;
    x ^= x >> 16;
    return (uint8_t)(x & 0xffu);
}

static void fill_payload(uint8_t *buffer, uint32_t size) {
    uint32_t i;
    for (i = 0u; i < size; ++i) buffer[i] = pattern_byte(size, i);
}

static int verify_payload(const uint8_t *buffer, uint32_t size) {
    uint32_t i;
    for (i = 0u; i < size; ++i) {
        if (buffer[i] != pattern_byte(size, i)) return 0;
    }
    return 1;
}

static DWORD WINAPI boundary_server(LPVOID opaque) {
    boundary_context *context = (boundary_context *)opaque;
    vipc_channel *channel = NULL;
    vipc_error error;
    vipc_status status;
    uint8_t *payload = NULL;
    uint32_t i;

    payload = (uint8_t *)malloc(VIPC_MAX_PAYLOAD_BYTES);
    if (!payload) {
        context->failed = 1;
        return 1u;
    }

    status = vipc_server_accept(context->server, 5000u, &channel, &error);
    if (status != VIPC_OK) {
        context->failed = 1;
        free(payload);
        return 1u;
    }

    for (i = 0u; i < SIZE_COUNT; ++i) {
        vipc_message request;
        vipc_message response;
        uint32_t received = 0u;

        status = vipc_channel_receive(
            channel,
            &request,
            payload,
            VIPC_MAX_PAYLOAD_BYTES,
            &received,
            10000u,
            &error);
        if (status != VIPC_OK
            || request.kind != VIPC_KIND_REQUEST
            || request.operation != VIPC_APP_OPERATION_MIN + 100u
            || request.correlation_id != (uint64_t)i + 1u
            || received != k_sizes[i]
            || !verify_payload(payload, received)) {
            context->failed = 1;
            break;
        }

        response.kind = VIPC_KIND_RESPONSE;
        response.flags = VIPC_FLAG_NONE;
        response.operation = request.operation;
        response.correlation_id = request.correlation_id;
        response.payload_size = received;

        status = vipc_channel_send(
            channel,
            &response,
            received ? payload : NULL,
            10000u,
            &error);
        if (status != VIPC_OK) {
            context->failed = 1;
            break;
        }
    }

    vipc_channel_destroy(channel);
    free(payload);
    return context->failed ? 1u : 0u;
}

int main(void) {
    char endpoint[64];
    vipc_server *server = NULL;
    vipc_channel *client = NULL;
    vipc_error error;
    vipc_status status;
    boundary_context context;
    HANDLE thread = NULL;
    DWORD thread_id = 0u;
    DWORD wait_result;
    uint8_t *send_payload = NULL;
    uint8_t *receive_payload = NULL;
    uint32_t i;
    int failed = 0;

    ZeroMemory(&context, sizeof(context));

    send_payload = (uint8_t *)malloc(VIPC_MAX_PAYLOAD_BYTES);
    receive_payload = (uint8_t *)malloc(VIPC_MAX_PAYLOAD_BYTES);
    if (!send_payload || !receive_payload) {
        free(send_payload);
        free(receive_payload);
        return 2;
    }

    (void)sprintf_s(
        endpoint,
        sizeof(endpoint),
        "boundary-%lu",
        (unsigned long)GetCurrentProcessId());

    status = vipc_server_create(endpoint, &server, &error);
    if (status != VIPC_OK) {
        failed = 1;
        goto cleanup;
    }

    context.server = server;
    thread = CreateThread(NULL, 0, boundary_server, &context, 0, &thread_id);
    if (!thread) {
        failed = 1;
        goto cleanup;
    }

    status = vipc_client_connect(endpoint, 5000u, &client, &error);
    if (status != VIPC_OK) {
        failed = 1;
        goto cleanup;
    }

    {
        vipc_message invalid;
        invalid.kind = VIPC_KIND_REQUEST;
        invalid.flags = VIPC_FLAG_NONE;
        invalid.operation = VIPC_APP_OPERATION_MIN + 100u;
        invalid.correlation_id = UINT64_C(0xdead);
        invalid.payload_size = VIPC_MAX_PAYLOAD_BYTES + 1u;
        status = vipc_channel_send(client, &invalid, send_payload, 100u, &error);
        if (status != VIPC_ERR_PAYLOAD_TOO_LARGE || !vipc_channel_is_open(client)) {
            fprintf(stderr, "oversize preflight failed: %s\n", vipc_status_name(status));
            failed = 1;
            goto cleanup;
        }

        invalid.payload_size = 1u;
        status = vipc_channel_send(client, &invalid, NULL, 100u, &error);
        if (status != VIPC_ERR_INVALID_ARGUMENT || !vipc_channel_is_open(client)) {
            fprintf(stderr, "NULL payload preflight failed: %s\n", vipc_status_name(status));
            failed = 1;
            goto cleanup;
        }
    }

    for (i = 0u; i < SIZE_COUNT; ++i) {
        vipc_message request;
        vipc_message response;
        uint32_t received = 0u;

        fill_payload(send_payload, k_sizes[i]);

        request.kind = VIPC_KIND_REQUEST;
        request.flags = VIPC_FLAG_NONE;
        request.operation = VIPC_APP_OPERATION_MIN + 100u;
        request.correlation_id = (uint64_t)i + 1u;
        request.payload_size = k_sizes[i];

        status = vipc_channel_send(
            client,
            &request,
            k_sizes[i] ? send_payload : NULL,
            10000u,
            &error);
        if (status != VIPC_OK) {
            fprintf(stderr, "send boundary %lu failed: %s\n",
                (unsigned long)k_sizes[i], vipc_status_name(status));
            failed = 1;
            break;
        }

        status = vipc_channel_receive(
            client,
            &response,
            receive_payload,
            VIPC_MAX_PAYLOAD_BYTES,
            &received,
            10000u,
            &error);
        if (status != VIPC_OK
            || response.kind != VIPC_KIND_RESPONSE
            || response.correlation_id != request.correlation_id
            || received != k_sizes[i]
            || memcmp(send_payload, receive_payload, received) != 0) {
            fprintf(stderr, "receive boundary %lu failed: %s\n",
                (unsigned long)k_sizes[i], vipc_status_name(status));
            failed = 1;
            break;
        }
    }

cleanup:
    if (client) vipc_channel_destroy(client);
    if (thread) {
        wait_result = WaitForSingleObject(thread, 15000u);
        if (wait_result != WAIT_OBJECT_0) failed = 1;
        (void)CloseHandle(thread);
    }
    if (context.failed) failed = 1;
    if (server) vipc_server_destroy(server);
    free(receive_payload);
    free(send_payload);

    if (failed) return 1;
    printf(
        "VectorIPC transport boundaries: PASS (%lu sizes through %u bytes)\n",
        (unsigned long)SIZE_COUNT,
        (unsigned)VIPC_MAX_PAYLOAD_BYTES);
    return 0;
}

#else
int main(void) {
    printf("VectorIPC transport boundaries: SKIP (Windows only)\n");
    return 0;
}
#endif
