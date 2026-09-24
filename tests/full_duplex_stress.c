#include "vectoripc/vipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define DEFAULT_MESSAGES 10000u
#define PAYLOAD_BYTES 32u

typedef struct accept_context {
    vipc_server *server;
    HANDLE ready;
    vipc_channel *channel;
    vipc_status status;
    vipc_error error;
} accept_context;

typedef struct stream_context {
    vipc_channel *channel;
    uint32_t count;
    uint16_t kind;
    uint32_t operation;
    uint32_t tag;
    int is_sender;
    volatile LONG failed;
} stream_context;

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

static void fill_payload(uint8_t payload[PAYLOAD_BYTES], uint32_t tag, uint32_t seq) {
    uint32_t i;
    for (i = 0u; i < PAYLOAD_BYTES; ++i) {
        payload[i] = (uint8_t)((tag * 97u + seq * 31u + i * 17u) & 0xffu);
    }
}

static DWORD WINAPI accept_main(LPVOID opaque) {
    accept_context *context = (accept_context *)opaque;
    context->status = vipc_server_accept(
        context->server,
        5000u,
        &context->channel,
        &context->error);
    (void)SetEvent(context->ready);
    return context->status == VIPC_OK ? 0u : 1u;
}

static DWORD WINAPI stream_main(LPVOID opaque) {
    stream_context *context = (stream_context *)opaque;
    uint32_t i;

    for (i = 0u; i < context->count; ++i) {
        vipc_error error;
        vipc_status status;
        uint8_t payload[PAYLOAD_BYTES];
        vipc_message message;

        fill_payload(payload, context->tag, i);

        if (context->is_sender) {
            message.kind = context->kind;
            message.flags = VIPC_FLAG_NONE;
            message.operation = context->operation;
            message.correlation_id = (uint64_t)i + 1u;
            message.payload_size = PAYLOAD_BYTES;
            status = vipc_channel_send(
                context->channel,
                &message,
                payload,
                5000u,
                &error);
        } else {
            uint8_t received_payload[PAYLOAD_BYTES];
            uint32_t received = 0u;
            status = vipc_channel_receive(
                context->channel,
                &message,
                received_payload,
                PAYLOAD_BYTES,
                &received,
                5000u,
                &error);
            if (status == VIPC_OK
                && (message.kind != context->kind
                    || message.operation != context->operation
                    || message.correlation_id != (uint64_t)i + 1u
                    || received != PAYLOAD_BYTES
                    || memcmp(received_payload, payload, PAYLOAD_BYTES) != 0)) {
                status = VIPC_ERR_PROTOCOL_INVALID;
            }
        }

        if (status != VIPC_OK) {
            (void)InterlockedExchange(&context->failed, 1);
            return 1u;
        }
    }
    return 0u;
}

int main(int argc, char **argv) {
    uint32_t count = DEFAULT_MESSAGES;
    char endpoint[64];
    vipc_server *server = NULL;
    vipc_channel *client = NULL;
    vipc_error error;
    vipc_status status;
    accept_context accepted;
    HANDLE accept_thread = NULL;
    HANDLE workers[4];
    stream_context contexts[4];
    DWORD thread_id = 0u;
    DWORD wait_result;
    uint32_t worker_count = 0u;
    uint32_t i;
    int failed = 0;

    if (argc == 2) {
        if (!parse_u32(argv[1], &count)) return 2;
    } else if (argc != 1) {
        fprintf(stderr, "usage: vectoripc_full_duplex_stress [messages-per-direction]\n");
        return 2;
    }

    ZeroMemory(&accepted, sizeof(accepted));
    ZeroMemory(workers, sizeof(workers));
    ZeroMemory(contexts, sizeof(contexts));


    (void)sprintf_s(
        endpoint,
        sizeof(endpoint),
        "duplex-stress-%lu",
        (unsigned long)GetCurrentProcessId());

    status = vipc_server_create(endpoint, &server, &error);
    if (status != VIPC_OK) return 1;

    accepted.server = server;
    accepted.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!accepted.ready) {
        vipc_server_destroy(server);
        return 1;
    }

    accept_thread = CreateThread(NULL, 0, accept_main, &accepted, 0, &thread_id);
    if (!accept_thread) {
        (void)CloseHandle(accepted.ready);
        vipc_server_destroy(server);
        return 1;
    }

    status = vipc_client_connect(endpoint, 5000u, &client, &error);
    if (status != VIPC_OK) {
        failed = 1;
        goto cleanup;
    }

    wait_result = WaitForSingleObject(accepted.ready, 5000u);
    if (wait_result != WAIT_OBJECT_0 || accepted.status != VIPC_OK || !accepted.channel) {
        failed = 1;
        goto cleanup;
    }

    contexts[0].channel = client;
    contexts[0].count = count;
    contexts[0].kind = VIPC_KIND_NOTIFY;
    contexts[0].operation = VIPC_APP_OPERATION_MIN + 110u;
    contexts[0].tag = 1u;
    contexts[0].is_sender = 1;

    contexts[1] = contexts[0];
    contexts[1].channel = accepted.channel;
    contexts[1].is_sender = 0;

    contexts[2].channel = accepted.channel;
    contexts[2].count = count;
    contexts[2].kind = VIPC_KIND_EVENT;
    contexts[2].operation = VIPC_APP_OPERATION_MIN + 111u;
    contexts[2].tag = 2u;
    contexts[2].is_sender = 1;

    contexts[3] = contexts[2];
    contexts[3].channel = client;
    contexts[3].is_sender = 0;

    for (i = 0u; i < 4u; ++i) {
        workers[i] = CreateThread(NULL, 0, stream_main, &contexts[i], 0, &thread_id);
        if (!workers[i]) {
            failed = 1;
            break;
        }
        worker_count += 1u;
    }

    if (!failed) {
        wait_result = WaitForMultipleObjects(4u, workers, TRUE, 30000u);
        if (wait_result != WAIT_OBJECT_0) failed = 1;
    }

    for (i = 0u; i < 4u; ++i) {
        if (contexts[i].failed) failed = 1;
    }

cleanup:
    for (i = 0u; i < worker_count; ++i) {
        (void)CloseHandle(workers[i]);
    }
    if (client) vipc_channel_destroy(client);
    if (accepted.channel) vipc_channel_destroy(accepted.channel);
    if (accept_thread) {
        (void)WaitForSingleObject(accept_thread, 1000u);
        (void)CloseHandle(accept_thread);
    }
    if (accepted.ready) (void)CloseHandle(accepted.ready);
    if (server) vipc_server_destroy(server);

    if (failed) return 1;
    printf(
        "VectorIPC full-duplex stress: PASS (%lu messages each direction)\n",
        (unsigned long)count);
    return 0;
}

#else
int main(void) {
    printf("VectorIPC full-duplex stress: SKIP (Windows only)\n");
    return 0;
}
#endif
