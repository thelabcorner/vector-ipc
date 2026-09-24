#include "vectoripc/vipc.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

typedef struct server_context {
    vipc_server *server;
    int failed;
} server_context;

typedef struct receive_context {
    vipc_channel *channel;
    HANDLE started;
    vipc_status status;
    vipc_error error;
    vipc_message message;
    uint8_t payload[64];
    uint32_t payload_size;
} receive_context;

static DWORD WINAPI receiver_thread(LPVOID opaque) {
    receive_context *context = (receive_context *)opaque;
    (void)SetEvent(context->started);
    context->status = vipc_channel_receive(
        context->channel,
        &context->message,
        context->payload,
        (uint32_t)sizeof(context->payload),
        &context->payload_size,
        2000u,
        &context->error);
    return 0u;
}

static DWORD WINAPI server_thread(LPVOID opaque) {
    server_context *context = (server_context *)opaque;
    vipc_channel *channel = NULL;
    vipc_error error;
    vipc_status status;
    vipc_message request;
    vipc_message outbound;
    uint8_t payload[64];
    uint32_t payload_size = 0;
    static const uint8_t response_payload[] = { 'r', 'e', 's', 'p' };
    static const uint8_t event_payload[] = { 'e', 'v', 't' };

    status = vipc_server_accept(
        context->server,
        2000u,
        &channel,
        &error);
    if (status != VIPC_OK) {
        context->failed = 1;
        return 1u;
    }

    status = vipc_channel_receive(
        channel,
        &request,
        payload,
        (uint32_t)sizeof(payload),
        &payload_size,
        2000u,
        &error);
    if (status != VIPC_OK
        || request.kind != VIPC_KIND_REQUEST
        || request.operation != VIPC_APP_OPERATION_MIN + 20u) {
        context->failed = 1;
        vipc_channel_destroy(channel);
        return 1u;
    }

    outbound.kind = VIPC_KIND_RESPONSE;
    outbound.flags = VIPC_FLAG_NONE;
    outbound.operation = request.operation;
    outbound.correlation_id = request.correlation_id;
    outbound.payload_size = (uint32_t)sizeof(response_payload);
    status = vipc_channel_send(
        channel,
        &outbound,
        response_payload,
        2000u,
        &error);
    if (status != VIPC_OK) {
        context->failed = 1;
        vipc_channel_destroy(channel);
        return 1u;
    }

    Sleep(100u);

    outbound.kind = VIPC_KIND_EVENT;
    outbound.flags = VIPC_FLAG_NONE;
    outbound.operation = VIPC_APP_OPERATION_MIN + 21u;
    outbound.correlation_id = 0u;
    outbound.payload_size = (uint32_t)sizeof(event_payload);
    status = vipc_channel_send(
        channel,
        &outbound,
        event_payload,
        2000u,
        &error);
    if (status != VIPC_OK) context->failed = 1;

    vipc_channel_destroy(channel);
    return context->failed ? 1u : 0u;
}

static int wait_thread(HANDLE thread, const char *label) {
    DWORD result = WaitForSingleObject(thread, 3000u);
    if (result != WAIT_OBJECT_0) {
        fprintf(stderr, "%s did not complete\n", label);
        return 0;
    }
    return 1;
}

int main(void) {
    char endpoint[64];
    vipc_server *server = NULL;
    vipc_channel *client = NULL;
    vipc_error error;
    vipc_status status;
    server_context server_ctx;
    receive_context receive_ctx;
    HANDLE server_handle = NULL;
    HANDLE receiver_handle = NULL;
    DWORD thread_id = 0;
    vipc_message request;
    vipc_message second_message;
    uint8_t second_payload[8];
    uint32_t second_payload_size = 0;
    int failed = 0;

    ZeroMemory(&receive_ctx, sizeof(receive_ctx));

    (void)sprintf_s(
        endpoint,
        sizeof(endpoint),
        "full-duplex-%lu",
        (unsigned long)GetCurrentProcessId());

    status = vipc_server_create(endpoint, &server, &error);
    if (status != VIPC_OK) {
        fprintf(stderr, "server create: %s\n", vipc_status_name(status));
        return 1;
    }

    server_ctx.server = server;
    server_ctx.failed = 0;
    server_handle = CreateThread(
        NULL, 0, server_thread, &server_ctx, 0, &thread_id);
    if (!server_handle) {
        vipc_server_destroy(server);
        return 1;
    }

    status = vipc_client_connect(endpoint, 2000u, &client, &error);
    if (status != VIPC_OK) {
        fprintf(stderr, "client connect: %s\n", vipc_status_name(status));
        failed = 1;
        goto cleanup;
    }

    receive_ctx.channel = client;
    receive_ctx.started = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!receive_ctx.started) {
        failed = 1;
        goto cleanup;
    }

    receiver_handle = CreateThread(
        NULL, 0, receiver_thread, &receive_ctx, 0, &thread_id);
    if (!receiver_handle) {
        failed = 1;
        goto cleanup;
    }

    if (WaitForSingleObject(receive_ctx.started, 1000u) != WAIT_OBJECT_0) {
        failed = 1;
        goto cleanup;
    }
    Sleep(5u);

    request.kind = VIPC_KIND_REQUEST;
    request.flags = VIPC_FLAG_NONE;
    request.operation = VIPC_APP_OPERATION_MIN + 20u;
    request.correlation_id = UINT64_C(0x7777);
    request.payload_size = 0u;

    status = vipc_channel_send(client, &request, NULL, 1000u, &error);
    if (status != VIPC_OK) {
        fprintf(stderr, "concurrent send failed: %s\n", vipc_status_name(status));
        failed = 1;
        goto cleanup;
    }

    if (!wait_thread(receiver_handle, "first receiver")) {
        failed = 1;
        goto cleanup;
    }
    (void)CloseHandle(receiver_handle);
    receiver_handle = NULL;

    if (receive_ctx.status != VIPC_OK
        || receive_ctx.message.kind != VIPC_KIND_RESPONSE
        || receive_ctx.message.correlation_id != request.correlation_id
        || receive_ctx.payload_size != 4u
        || memcmp(receive_ctx.payload, "resp", 4u) != 0) {
        fprintf(stderr, "full-duplex response mismatch\n");
        failed = 1;
        goto cleanup;
    }

    (void)ResetEvent(receive_ctx.started);
    receive_ctx.status = VIPC_OK;
    receive_ctx.payload_size = 0u;
    receiver_handle = CreateThread(
        NULL, 0, receiver_thread, &receive_ctx, 0, &thread_id);
    if (!receiver_handle) {
        failed = 1;
        goto cleanup;
    }
    if (WaitForSingleObject(receive_ctx.started, 1000u) != WAIT_OBJECT_0) {
        failed = 1;
        goto cleanup;
    }
    Sleep(5u);

    status = vipc_channel_receive(
        client,
        &second_message,
        second_payload,
        (uint32_t)sizeof(second_payload),
        &second_payload_size,
        10u,
        &error);
    if (status != VIPC_ERR_BUSY) {
        fprintf(stderr, "second reader was not rejected: %s\n",
            vipc_status_name(status));
        failed = 1;
        goto cleanup;
    }

    if (!wait_thread(receiver_handle, "second receiver")) {
        failed = 1;
        goto cleanup;
    }
    (void)CloseHandle(receiver_handle);
    receiver_handle = NULL;

    if (receive_ctx.status != VIPC_OK
        || receive_ctx.message.kind != VIPC_KIND_EVENT
        || receive_ctx.message.operation != VIPC_APP_OPERATION_MIN + 21u
        || receive_ctx.payload_size != 3u
        || memcmp(receive_ctx.payload, "evt", 3u) != 0) {
        fprintf(stderr, "event receive mismatch\n");
        failed = 1;
    }

cleanup:
    if (receiver_handle) {
        (void)WaitForSingleObject(receiver_handle, 2500u);
        (void)CloseHandle(receiver_handle);
    }
    if (receive_ctx.started) (void)CloseHandle(receive_ctx.started);
    if (client) vipc_channel_destroy(client);
    if (server_handle) {
        if (!wait_thread(server_handle, "server")) failed = 1;
        (void)CloseHandle(server_handle);
    }
    if (server_ctx.failed) failed = 1;
    vipc_server_destroy(server);

    if (failed) return 1;
    printf("VectorIPC full-duplex test: PASS\n");
    return 0;
}

#else

int main(void) {
    printf("VectorIPC full-duplex test: SKIP (Windows transport only)\n");
    return 0;
}

#endif