#include "vectoripc/vipc.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define STRESS_FRAMES 20000u
#define STRESS_MAX_PAYLOAD 4096u

typedef struct stress_context {
    vipc_server *server;
    int failed;
} stress_context;

static uint32_t pattern_size(uint32_t sequence) {
    uint32_t x = sequence * 1664525u + 1013904223u;
    return x % (STRESS_MAX_PAYLOAD + 1u);
}

static uint8_t pattern_byte(uint32_t sequence, uint32_t index) {
    uint32_t x = sequence * 2246822519u;
    x ^= index * 3266489917u;
    x ^= x >> 13;
    return (uint8_t)(x & 0xffu);
}

static void fill_payload(uint8_t *buffer, uint32_t size, uint32_t sequence) {
    uint32_t i;
    for (i = 0; i < size; ++i) buffer[i] = pattern_byte(sequence, i);
}

static int verify_payload(
    const uint8_t *buffer,
    uint32_t size,
    uint32_t sequence) {
    uint32_t i;
    for (i = 0; i < size; ++i) {
        if (buffer[i] != pattern_byte(sequence, i)) return 0;
    }
    return 1;
}

static DWORD WINAPI stress_server(LPVOID opaque) {
    stress_context *context = (stress_context *)opaque;
    vipc_channel *channel = NULL;
    vipc_error error;
    vipc_status status;
    vipc_message inbound;
    vipc_message outbound;
    uint8_t payload[STRESS_MAX_PAYLOAD];
    uint32_t payload_size = 0;
    uint32_t i;

    status = vipc_server_accept(
        context->server,
        3000u,
        &channel,
        &error);
    if (status != VIPC_OK) {
        context->failed = 1;
        return 1u;
    }

    for (i = 0; i < STRESS_FRAMES; ++i) {
        status = vipc_channel_receive(
            channel,
            &inbound,
            payload,
            (uint32_t)sizeof(payload),
            &payload_size,
            3000u,
            &error);
        if (status != VIPC_OK
            || inbound.kind != VIPC_KIND_REQUEST
            || inbound.operation != VIPC_APP_OPERATION_MIN + 30u
            || inbound.correlation_id != (uint64_t)i + 1u
            || payload_size != pattern_size(i)
            || !verify_payload(payload, payload_size, i)) {
            context->failed = 1;
            break;
        }

        outbound.kind = VIPC_KIND_RESPONSE;
        outbound.flags = VIPC_FLAG_NONE;
        outbound.operation = inbound.operation;
        outbound.correlation_id = inbound.correlation_id;
        outbound.payload_size = payload_size;
        status = vipc_channel_send(
            channel,
            &outbound,
            payload_size ? payload : NULL,
            3000u,
            &error);
        if (status != VIPC_OK) {
            context->failed = 1;
            break;
        }
    }

    vipc_channel_destroy(channel);
    return context->failed ? 1u : 0u;
}

int main(void) {
    char endpoint[64];
    vipc_server *server = NULL;
    vipc_channel *client = NULL;
    vipc_error error;
    vipc_status status;
    stress_context context;
    HANDLE server_thread = NULL;
    DWORD thread_id = 0;
    uint8_t send_payload[STRESS_MAX_PAYLOAD];
    uint8_t receive_payload[STRESS_MAX_PAYLOAD];
    uint32_t i;
    int failed = 0;

    (void)sprintf_s(
        endpoint,
        sizeof(endpoint),
        "stress-%lu",
        (unsigned long)GetCurrentProcessId());

    status = vipc_server_create(endpoint, &server, &error);
    if (status != VIPC_OK) return 1;

    context.server = server;
    context.failed = 0;
    server_thread = CreateThread(
        NULL, 0, stress_server, &context, 0, &thread_id);
    if (!server_thread) {
        vipc_server_destroy(server);
        return 1;
    }

    status = vipc_client_connect(endpoint, 3000u, &client, &error);
    if (status != VIPC_OK) {
        failed = 1;
        goto cleanup;
    }

    for (i = 0; i < STRESS_FRAMES; ++i) {
        vipc_message outbound;
        vipc_message inbound;
        uint32_t size = pattern_size(i);
        uint32_t received = 0;

        fill_payload(send_payload, size, i);

        outbound.kind = VIPC_KIND_REQUEST;
        outbound.flags = VIPC_FLAG_NONE;
        outbound.operation = VIPC_APP_OPERATION_MIN + 30u;
        outbound.correlation_id = (uint64_t)i + 1u;
        outbound.payload_size = size;

        status = vipc_channel_send(
            client,
            &outbound,
            size ? send_payload : NULL,
            3000u,
            &error);
        if (status != VIPC_OK) {
            fprintf(stderr, "send failed at %lu: %s\n",
                (unsigned long)i, vipc_status_name(status));
            failed = 1;
            break;
        }

        status = vipc_channel_receive(
            client,
            &inbound,
            receive_payload,
            (uint32_t)sizeof(receive_payload),
            &received,
            3000u,
            &error);
        if (status != VIPC_OK
            || inbound.kind != VIPC_KIND_RESPONSE
            || inbound.operation != outbound.operation
            || inbound.correlation_id != outbound.correlation_id
            || received != size
            || memcmp(send_payload, receive_payload, size) != 0) {
            fprintf(stderr, "receive mismatch at %lu: %s\n",
                (unsigned long)i, vipc_status_name(status));
            failed = 1;
            break;
        }
    }

cleanup:
    if (client) vipc_channel_destroy(client);
    if (server_thread) {
        DWORD wait_result = WaitForSingleObject(server_thread, 5000u);
        if (wait_result != WAIT_OBJECT_0) failed = 1;
        (void)CloseHandle(server_thread);
    }
    if (context.failed) failed = 1;
    vipc_server_destroy(server);

    if (failed) return 1;
    printf("VectorIPC transport stress: PASS (%lu variable-size round trips)\n",
        (unsigned long)STRESS_FRAMES);
    return 0;
}

#else

int main(void) {
    printf("VectorIPC transport stress: SKIP (Windows transport only)\n");
    return 0;
}

#endif