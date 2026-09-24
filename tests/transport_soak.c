#include "vectoripc/vipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define DEFAULT_FRAMES 100000u
#define DEFAULT_MAX_PAYLOAD 4096u
#define SOAK_TIMEOUT_MS 10000u

typedef struct soak_context {
    vipc_server *server;
    uint32_t frames;
    uint32_t max_payload;
    int failed;
} soak_context;

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

static uint32_t pattern_size(uint32_t sequence, uint32_t max_payload) {
    uint32_t x = sequence * 1664525u + 1013904223u;
    return x % (max_payload + 1u);
}

static uint8_t pattern_byte(uint32_t sequence, uint32_t index) {
    uint32_t x = sequence * 2246822519u;
    x ^= index * 3266489917u;
    x ^= x >> 13;
    return (uint8_t)(x & 0xffu);
}

static void fill_payload(uint8_t *buffer, uint32_t size, uint32_t sequence) {
    uint32_t i;
    for (i = 0u; i < size; ++i) buffer[i] = pattern_byte(sequence, i);
}

static int verify_payload(
    const uint8_t *buffer,
    uint32_t size,
    uint32_t sequence) {
    uint32_t i;
    for (i = 0u; i < size; ++i) {
        if (buffer[i] != pattern_byte(sequence, i)) return 0;
    }
    return 1;
}

static DWORD WINAPI soak_server(LPVOID opaque) {
    soak_context *context = (soak_context *)opaque;
    vipc_channel *channel = NULL;
    vipc_error error;
    vipc_status status;
    uint8_t *payload = NULL;
    uint32_t i;

    payload = (uint8_t *)malloc(context->max_payload);
    if (!payload) {
        context->failed = 1;
        return 1u;
    }

    status = vipc_server_accept(
        context->server,
        SOAK_TIMEOUT_MS,
        &channel,
        &error);
    if (status != VIPC_OK) {
        context->failed = 1;
        free(payload);
        return 1u;
    }

    for (i = 0u; i < context->frames; ++i) {
        vipc_message request;
        vipc_message response;
        uint32_t received = 0u;
        uint32_t expected = pattern_size(i, context->max_payload);

        status = vipc_channel_receive(
            channel,
            &request,
            payload,
            context->max_payload,
            &received,
            SOAK_TIMEOUT_MS,
            &error);
        if (status != VIPC_OK
            || request.kind != VIPC_KIND_REQUEST
            || request.operation != VIPC_APP_OPERATION_MIN + 120u
            || request.correlation_id != (uint64_t)i + 1u
            || received != expected
            || !verify_payload(payload, received, i)) {
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
            SOAK_TIMEOUT_MS,
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

int main(int argc, char **argv) {
    uint32_t frames = DEFAULT_FRAMES;
    uint32_t max_payload = DEFAULT_MAX_PAYLOAD;
    char endpoint[64];
    vipc_server *server = NULL;
    vipc_channel *client = NULL;
    vipc_error error;
    vipc_status status;
    soak_context context;
    HANDLE thread = NULL;
    DWORD thread_id = 0u;
    DWORD wait_result;
    LARGE_INTEGER frequency;
    LARGE_INTEGER before = { 0 };
    LARGE_INTEGER after = { 0 };
    uint8_t *send_payload = NULL;
    uint8_t *receive_payload = NULL;
    uint64_t total_user_bytes = 0u;
    uint32_t i;
    int failed = 0;

    if (argc >= 2 && !parse_u32(argv[1], &frames)) return 2;
    if (argc >= 3 && !parse_u32(argv[2], &max_payload)) return 2;
    if (argc > 3 || max_payload > VIPC_MAX_PAYLOAD_BYTES) {
        fprintf(stderr, "usage: vectoripc_transport_soak [frames] [max-payload<=%u]\n",
            (unsigned)VIPC_MAX_PAYLOAD_BYTES);
        return 2;
    }

    if (!QueryPerformanceFrequency(&frequency)) return 2;
    send_payload = (uint8_t *)malloc(max_payload);
    receive_payload = (uint8_t *)malloc(max_payload);
    if (!send_payload || !receive_payload) {
        free(send_payload);
        free(receive_payload);
        return 2;
    }

    ZeroMemory(&context, sizeof(context));
    context.frames = frames;
    context.max_payload = max_payload;

    (void)sprintf_s(
        endpoint,
        sizeof(endpoint),
        "soak-%lu",
        (unsigned long)GetCurrentProcessId());

    status = vipc_server_create(endpoint, &server, &error);
    if (status != VIPC_OK) {
        failed = 1;
        goto cleanup;
    }
    context.server = server;

    thread = CreateThread(NULL, 0, soak_server, &context, 0, &thread_id);
    if (!thread) {
        failed = 1;
        goto cleanup;
    }

    status = vipc_client_connect(endpoint, SOAK_TIMEOUT_MS, &client, &error);
    if (status != VIPC_OK) {
        failed = 1;
        goto cleanup;
    }

    QueryPerformanceCounter(&before);
    for (i = 0u; i < frames; ++i) {
        vipc_message request;
        vipc_message response;
        uint32_t size = pattern_size(i, max_payload);
        uint32_t received = 0u;

        fill_payload(send_payload, size, i);

        request.kind = VIPC_KIND_REQUEST;
        request.flags = VIPC_FLAG_NONE;
        request.operation = VIPC_APP_OPERATION_MIN + 120u;
        request.correlation_id = (uint64_t)i + 1u;
        request.payload_size = size;

        status = vipc_channel_send(
            client,
            &request,
            size ? send_payload : NULL,
            SOAK_TIMEOUT_MS,
            &error);
        if (status != VIPC_OK) {
            failed = 1;
            break;
        }

        status = vipc_channel_receive(
            client,
            &response,
            receive_payload,
            max_payload,
            &received,
            SOAK_TIMEOUT_MS,
            &error);
        if (status != VIPC_OK
            || response.kind != VIPC_KIND_RESPONSE
            || response.operation != request.operation
            || response.correlation_id != request.correlation_id
            || received != size
            || memcmp(send_payload, receive_payload, size) != 0) {
            failed = 1;
            break;
        }

        total_user_bytes += (uint64_t)size * 2u;
    }
    QueryPerformanceCounter(&after);

cleanup:
    if (client) vipc_channel_destroy(client);
    if (thread) {
        wait_result = WaitForSingleObject(thread, 30000u);
        if (wait_result != WAIT_OBJECT_0) failed = 1;
        (void)CloseHandle(thread);
    }
    if (context.failed) failed = 1;
    if (server) vipc_server_destroy(server);

    if (!failed) {
        double seconds = (double)(after.QuadPart - before.QuadPart)
            / (double)frequency.QuadPart;
        double rps = (double)frames / seconds;
        double mib_s = ((double)total_user_bytes / (1024.0 * 1024.0)) / seconds;
        printf(
            "VectorIPC transport soak: PASS frames=%lu max_payload=%lu "
            "seconds=%.3f rps=%.0f verified_duplex_mib_s=%.1f\n",
            (unsigned long)frames,
            (unsigned long)max_payload,
            seconds,
            rps,
            mib_s);
    }

    free(receive_payload);
    free(send_payload);
    return failed ? 1 : 0;
}

#else
int main(void) {
    printf("VectorIPC transport soak: SKIP (Windows only)\n");
    return 0;
}
#endif
