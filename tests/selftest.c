#include "vectoripc/vipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

static int g_failures = 0;

#define CHECK(condition, label) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", (label), __LINE__); \
        ++g_failures; \
    } \
} while (0)

static void test_abi_contract(void) {
    uint32_t version = vipc_abi_version();
    CHECK(version == VIPC_ABI_VERSION, "ABI version");
}

static void test_protocol(void) {
    vipc_message message;
    vipc_message decoded;
    uint8_t header[VIPC_WIRE_HEADER_SIZE];
    uint8_t bad[VIPC_WIRE_HEADER_SIZE];
    vipc_protocol_status status;
    static const uint8_t expected_header[VIPC_WIRE_HEADER_SIZE] = {
        0x56u, 0x49u, 0x50u, 0x43u,
        0x01u, 0x00u,
        0x00u, 0x00u,
        0x20u, 0x00u,
        0x01u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u,
        0x07u, 0x01u, 0x00u, 0x00u,
        0x88u, 0x77u, 0x66u, 0x55u,
        0x44u, 0x33u, 0x22u, 0x11u,
        0xd2u, 0x04u, 0x00u, 0x00u
    };

    message.kind = VIPC_KIND_REQUEST;
    message.flags = VIPC_FLAG_NONE;
    message.operation = VIPC_APP_OPERATION_MIN + 7u;
    message.correlation_id = UINT64_C(0x1122334455667788);
    message.payload_size = 1234u;

    status = vipc_protocol_encode(&message, header);
    CHECK(status == VIPC_PROTOCOL_OK, "protocol encode");
    CHECK(memcmp(header, expected_header, sizeof(expected_header)) == 0,
        "golden wire vector");
    CHECK(header[0] == 'V' && header[1] == 'I'
        && header[2] == 'P' && header[3] == 'C', "wire magic");
    CHECK(header[8] == 32u && header[9] == 0u, "header size encoding");

    status = vipc_protocol_decode(header, &decoded);
    CHECK(status == VIPC_PROTOCOL_OK, "protocol decode");
    CHECK(decoded.kind == message.kind, "kind round-trip");
    CHECK(decoded.flags == message.flags, "flags round-trip");
    CHECK(decoded.operation == message.operation, "operation round-trip");
    CHECK(decoded.correlation_id == message.correlation_id, "correlation round-trip");
    CHECK(decoded.payload_size == message.payload_size, "payload size round-trip");

    memcpy(bad, header, sizeof(bad));
    bad[0] = 'X';
    CHECK(vipc_protocol_decode(bad, &decoded) == VIPC_PROTOCOL_BAD_MAGIC,
        "bad magic rejected");

    memcpy(bad, header, sizeof(bad));
    bad[4] = 2u;
    CHECK(vipc_protocol_decode(bad, &decoded) == VIPC_PROTOCOL_BAD_VERSION,
        "bad major rejected");

    memcpy(bad, header, sizeof(bad));
    bad[8] = 31u;
    CHECK(vipc_protocol_decode(bad, &decoded) == VIPC_PROTOCOL_BAD_HEADER_SIZE,
        "bad header size rejected");

    decoded.kind = UINT32_C(0xaaaaaaaa);
    decoded.flags = UINT32_C(0xbbbbbbbb);
    decoded.operation = UINT32_C(0xcccccccc);
    decoded.payload_size = UINT32_C(0xdddddddd);
    decoded.correlation_id = UINT64_C(0xeeeeeeeeeeeeeeee);
    memcpy(bad, header, sizeof(bad));
    bad[10] = 99u;
    CHECK(vipc_protocol_decode(bad, &decoded) == VIPC_PROTOCOL_BAD_KIND,
        "bad kind rejected");
    CHECK(decoded.kind == UINT32_C(0xaaaaaaaa)
        && decoded.flags == UINT32_C(0xbbbbbbbb)
        && decoded.operation == UINT32_C(0xcccccccc)
        && decoded.payload_size == UINT32_C(0xdddddddd)
        && decoded.correlation_id == UINT64_C(0xeeeeeeeeeeeeeeee),
        "failed decode leaves output untouched");

    memcpy(bad, header, sizeof(bad));
    bad[12] = 0x80u;
    CHECK(vipc_protocol_decode(bad, &decoded) == VIPC_PROTOCOL_BAD_FLAGS,
        "unknown flags rejected");

    message.payload_size = VIPC_MAX_PAYLOAD_BYTES + 1u;
    CHECK(vipc_protocol_validate(&message) == VIPC_PROTOCOL_PAYLOAD_TOO_LARGE,
        "oversized payload rejected");
}

static void test_endpoint_validation(void) {
    char too_long[VIPC_ENDPOINT_MAX + 2u];
    size_t i;

    CHECK(vipc_endpoint_is_valid("sample-app") == 1, "simple endpoint");
    CHECK(vipc_endpoint_is_valid("com.example_tool-1") == 1, "compound endpoint");
    CHECK(vipc_endpoint_is_valid("") == 0, "empty endpoint rejected");
    CHECK(vipc_endpoint_is_valid("bad/endpoint") == 0, "slash rejected");
    CHECK(vipc_endpoint_is_valid("bad\\endpoint") == 0, "backslash rejected");
    CHECK(vipc_endpoint_is_valid("bad endpoint") == 0, "space rejected");

    for (i = 0; i < VIPC_ENDPOINT_MAX + 1u; ++i) too_long[i] = 'a';
    too_long[VIPC_ENDPOINT_MAX + 1u] = '\0';
    CHECK(vipc_endpoint_is_valid(too_long) == 0, "long endpoint rejected");
}

#ifdef _WIN32

typedef struct server_context {
    vipc_server *server;
    int failures;
} server_context;

static void server_check(server_context *context, int condition, const char *label) {
    if (!condition) {
        fprintf(stderr, "SERVER FAIL: %s\n", label);
        ++context->failures;
    }
}

static DWORD WINAPI server_thread_main(LPVOID opaque) {
    server_context *context = (server_context *)opaque;
    vipc_channel *channel = NULL;
    vipc_error error;
    vipc_status status;
    vipc_message message;
    vipc_message response;
    uint8_t payload[64];
    uint32_t payload_size = 0;
    static const uint8_t expected_request[] = { 0u, 1u, 2u, 0xffu, 3u };
    static const uint8_t response_payload[] = { 3u, 0xffu, 2u, 1u, 0u };
    static const uint8_t drain_payload[] = { 10u, 11u, 12u, 13u, 14u, 15u, 16u, 17u };
    static const uint8_t event_payload[] = { 'o', 'k' };

    status = vipc_server_accept(
        context->server,
        2000u,
        &channel,
        &error);
    server_check(context, status == VIPC_OK, "accept client");
    if (status != VIPC_OK) return 1u;

    server_check(
        context,
        vipc_channel_peer_pid(channel) == (uint32_t)GetCurrentProcessId(),
        "server peer pid");

    status = vipc_channel_receive(
        channel,
        &message,
        payload,
        (uint32_t)sizeof(payload),
        &payload_size,
        2000u,
        &error);
    server_check(context, status == VIPC_OK, "receive binary request");
    server_check(context, message.kind == VIPC_KIND_REQUEST, "request kind");
    server_check(context, message.operation == VIPC_APP_OPERATION_MIN + 1u, "request op");
    server_check(context, message.correlation_id == UINT64_C(0xabc), "request correlation");
    server_check(context, payload_size == (uint32_t)sizeof(expected_request), "request size");
    server_check(
        context,
        payload_size == (uint32_t)sizeof(expected_request)
            && memcmp(payload, expected_request, sizeof(expected_request)) == 0,
        "binary payload including NUL/FF");

    response.kind = VIPC_KIND_RESPONSE;
    response.flags = VIPC_FLAG_NONE;
    response.operation = message.operation;
    response.correlation_id = message.correlation_id;
    response.payload_size = (uint32_t)sizeof(response_payload);
    status = vipc_channel_send(
        channel,
        &response,
        response_payload,
        2000u,
        &error);
    server_check(context, status == VIPC_OK, "send response");

    response.kind = VIPC_KIND_EVENT;
    response.flags = VIPC_FLAG_NONE;
    response.operation = VIPC_APP_OPERATION_MIN + 2u;
    response.correlation_id = 0u;
    response.payload_size = (uint32_t)sizeof(drain_payload);
    status = vipc_channel_send(
        channel,
        &response,
        drain_payload,
        2000u,
        &error);
    server_check(context, status == VIPC_OK, "send drain test frame");

    response.kind = VIPC_KIND_EVENT;
    response.flags = VIPC_FLAG_NONE;
    response.operation = VIPC_APP_OPERATION_MIN + 3u;
    response.correlation_id = UINT64_C(0xdef);
    response.payload_size = (uint32_t)sizeof(event_payload);
    status = vipc_channel_send(
        channel,
        &response,
        event_payload,
        2000u,
        &error);
    server_check(context, status == VIPC_OK, "send post-drain frame");

    status = vipc_channel_receive(
        channel,
        &message,
        payload,
        (uint32_t)sizeof(payload),
        &payload_size,
        2000u,
        &error);
    server_check(context, status == VIPC_OK, "receive delay request");
    server_check(context, message.operation == VIPC_APP_OPERATION_MIN + 4u, "delay request op");

    Sleep(150u);

    response.kind = VIPC_KIND_RESPONSE;
    response.flags = VIPC_FLAG_NONE;
    response.operation = message.operation;
    response.correlation_id = message.correlation_id;
    response.payload_size = 0u;
    (void)vipc_channel_send(channel, &response, NULL, 250u, &error);

    vipc_channel_destroy(channel);
    return 0u;
}

static void test_windows_transport(void) {
    char endpoint[64];
    char missing_endpoint[64];
    vipc_server *server = NULL;
    vipc_server *duplicate_server = NULL;
    vipc_channel *client = NULL;
    vipc_channel *unexpected = NULL;
    vipc_error error;
    vipc_status status;
    server_context context;
    HANDLE thread;
    DWORD thread_id = 0;
    DWORD wait_result;
    vipc_message message;
    vipc_message received;
    uint8_t payload[64];
    uint8_t tiny[2];
    uint32_t payload_size = 0;
    ULONGLONG started;
    ULONGLONG elapsed;
    static const uint8_t request_payload[] = { 0u, 1u, 2u, 0xffu, 3u };
    static const uint8_t expected_response[] = { 3u, 0xffu, 2u, 1u, 0u };

    (void)sprintf_s(
        endpoint,
        sizeof(endpoint),
        "selftest-%lu",
        (unsigned long)GetCurrentProcessId());

    status = vipc_server_create(endpoint, &server, &error);
    CHECK(status == VIPC_OK, "server create");
    if (status != VIPC_OK) return;

    status = vipc_server_create(endpoint, &duplicate_server, &error);
    CHECK(status == VIPC_ERR_ENDPOINT_IN_USE, "duplicate server rejected");
    CHECK(duplicate_server == NULL, "duplicate server no handle");

    (void)sprintf_s(
        missing_endpoint,
        sizeof(missing_endpoint),
        "missing-%lu",
        (unsigned long)GetCurrentProcessId());
    status = vipc_client_connect(
        missing_endpoint,
        5u,
        &unexpected,
        &error);
    CHECK(status == VIPC_ERR_ENDPOINT_NOT_FOUND, "missing endpoint classified");
    CHECK(unexpected == NULL, "missing endpoint no channel");

    /*
     * Exercise accept timeout cancellation and verify the listener remains
     * usable afterward.
     */
    status = vipc_server_accept(server, 5u, &unexpected, &error);
    CHECK(status == VIPC_ERR_TIMEOUT, "accept timeout");
    CHECK(unexpected == NULL, "accept timeout no channel");

    context.server = server;
    context.failures = 0;
    thread = CreateThread(
        NULL,
        0,
        server_thread_main,
        &context,
        0,
        &thread_id);
    CHECK(thread != NULL, "server thread create");
    if (!thread) {
        vipc_server_destroy(server);
        return;
    }

    status = vipc_client_connect(endpoint, 2000u, &client, &error);
    CHECK(status == VIPC_OK, "client connect");
    if (status != VIPC_OK) {
        (void)WaitForSingleObject(thread, 2500u);
        (void)CloseHandle(thread);
        vipc_server_destroy(server);
        return;
    }

    CHECK(vipc_channel_is_open(client) == 1, "client open");
    CHECK(
        vipc_channel_peer_pid(client) == (uint32_t)GetCurrentProcessId(),
        "client peer pid");

    message.kind = VIPC_KIND_REQUEST;
    message.flags = VIPC_FLAG_NONE;
    message.operation = VIPC_APP_OPERATION_MIN + 1u;
    message.correlation_id = UINT64_C(0xabc);
    message.payload_size = (uint32_t)sizeof(request_payload);
    status = vipc_channel_send(
        client,
        &message,
        request_payload,
        2000u,
        &error);
    CHECK(status == VIPC_OK, "client send binary request");

    status = vipc_channel_receive(
        client,
        &received,
        payload,
        (uint32_t)sizeof(payload),
        &payload_size,
        2000u,
        &error);
    CHECK(status == VIPC_OK, "client receive response");
    CHECK(received.kind == VIPC_KIND_RESPONSE, "response kind");
    CHECK(received.operation == message.operation, "response op");
    CHECK(received.correlation_id == message.correlation_id, "response correlation");
    CHECK(payload_size == (uint32_t)sizeof(expected_response), "response size");
    CHECK(
        payload_size == (uint32_t)sizeof(expected_response)
            && memcmp(payload, expected_response, sizeof(expected_response)) == 0,
        "response payload");

    status = vipc_channel_receive(
        client,
        &received,
        tiny,
        (uint32_t)sizeof(tiny),
        &payload_size,
        2000u,
        &error);
    CHECK(status == VIPC_ERR_BUFFER_TOO_SMALL, "small buffer reported");
    CHECK(received.payload_size == 8u, "small buffer required size");
    CHECK(payload_size == 0u, "small buffer copies zero");

    status = vipc_channel_receive(
        client,
        &received,
        payload,
        (uint32_t)sizeof(payload),
        &payload_size,
        2000u,
        &error);
    CHECK(status == VIPC_OK, "stream resynchronized after drain");
    CHECK(received.kind == VIPC_KIND_EVENT, "event kind");
    CHECK(received.operation == VIPC_APP_OPERATION_MIN + 3u, "event op");
    CHECK(payload_size == 2u && payload[0] == 'o' && payload[1] == 'k',
        "event payload after drain");

    message.kind = VIPC_KIND_REQUEST;
    message.flags = VIPC_FLAG_NONE;
    message.operation = VIPC_APP_OPERATION_MIN + 4u;
    message.correlation_id = UINT64_C(0x1234);
    message.payload_size = 0u;
    status = vipc_channel_send(client, &message, NULL, 1000u, &error);
    CHECK(status == VIPC_OK, "send timeout test request");

    started = GetTickCount64();
    status = vipc_channel_receive(
        client,
        &received,
        payload,
        (uint32_t)sizeof(payload),
        &payload_size,
        30u,
        &error);
    elapsed = GetTickCount64() - started;
    CHECK(status == VIPC_ERR_TIMEOUT, "receive hard timeout");
    CHECK(elapsed < 150u, "timeout remains bounded");
    CHECK(vipc_channel_is_open(client) == 0, "timeout poisons stream");

    vipc_channel_destroy(client);
    client = NULL;

    wait_result = WaitForSingleObject(thread, 2500u);
    CHECK(wait_result == WAIT_OBJECT_0, "server thread exits");
    if (wait_result == WAIT_OBJECT_0) {
        DWORD exit_code = 1u;
        (void)GetExitCodeThread(thread, &exit_code);
        CHECK(exit_code == 0u, "server thread exit code");
    }
    (void)CloseHandle(thread);

    CHECK(context.failures == 0, "server assertions");
    vipc_server_destroy(server);
}

typedef struct blocked_send_context {
    vipc_channel *channel;
    HANDLE entered;
    const uint8_t *payload;
    uint32_t payload_size;
    vipc_status status;
    vipc_error error;
} blocked_send_context;

static DWORD WINAPI blocked_send_main(LPVOID opaque) {
    blocked_send_context *context = (blocked_send_context *)opaque;
    vipc_message message;

    message.kind = VIPC_KIND_NOTIFY;
    message.flags = VIPC_FLAG_NONE;
    message.operation = VIPC_APP_OPERATION_MIN + 30u;
    message.correlation_id = 0u;
    message.payload_size = context->payload_size;

    (void)SetEvent(context->entered);
    context->status = vipc_channel_send(
        context->channel,
        &message,
        context->payload,
        2000u,
        &context->error);
    return 0u;
}

typedef struct slow_reader_context {
    vipc_server *server;
    int failures;
} slow_reader_context;

static DWORD WINAPI slow_reader_main(LPVOID opaque) {
    slow_reader_context *context = (slow_reader_context *)opaque;
    vipc_channel *channel = NULL;
    vipc_error error;
    vipc_status status;
    vipc_message message;
    uint8_t *payload = NULL;
    uint32_t payload_size = 0;
    const uint32_t expected_size = 1024u * 1024u;

    status = vipc_server_accept(
        context->server,
        2000u,
        &channel,
        &error);
    if (status != VIPC_OK) {
        ++context->failures;
        return 1u;
    }

    /*
     * Let the sender fill the kernel pipe buffer first. A 1 MiB frame cannot
     * finish into the configured 64 KiB pipe buffer without a reader.
     */
    Sleep(100u);

    payload = (uint8_t *)malloc(expected_size);
    if (!payload) {
        ++context->failures;
        vipc_channel_destroy(channel);
        return 1u;
    }

    status = vipc_channel_receive(
        channel,
        &message,
        payload,
        expected_size,
        &payload_size,
        2000u,
        &error);
    if (status != VIPC_OK
        || message.operation != VIPC_APP_OPERATION_MIN + 30u
        || payload_size != expected_size) {
        ++context->failures;
    }

    free(payload);
    vipc_channel_destroy(channel);
    return context->failures == 0 ? 0u : 1u;
}

static void test_concurrent_write_busy(void) {
    char endpoint[64];
    vipc_server *server = NULL;
    vipc_channel *client = NULL;
    vipc_error error;
    vipc_status status;
    slow_reader_context server_context;
    blocked_send_context send_context;
    HANDLE server_thread = NULL;
    HANDLE sender_thread = NULL;
    DWORD thread_id = 0;
    DWORD wait_result;
    uint8_t *payload = NULL;
    vipc_message second_message;
    uint32_t i;

    ZeroMemory(&server_context, sizeof(server_context));
    ZeroMemory(&send_context, sizeof(send_context));

    (void)sprintf_s(
        endpoint,
        sizeof(endpoint),
        "writebusy-%lu",
        (unsigned long)GetCurrentProcessId());

    status = vipc_server_create(endpoint, &server, &error);
    CHECK(status == VIPC_OK, "write-busy server create");
    if (status != VIPC_OK) return;

    server_context.server = server;
    server_thread = CreateThread(
        NULL,
        0,
        slow_reader_main,
        &server_context,
        0,
        &thread_id);
    CHECK(server_thread != NULL, "write-busy server thread");
    if (!server_thread) goto cleanup;

    status = vipc_client_connect(endpoint, 2000u, &client, &error);
    CHECK(status == VIPC_OK, "write-busy client connect");
    if (status != VIPC_OK) goto cleanup;

    payload = (uint8_t *)malloc(1024u * 1024u);
    CHECK(payload != NULL, "write-busy payload alloc");
    if (!payload) goto cleanup;
    for (i = 0; i < 1024u * 1024u; ++i) {
        payload[i] = (uint8_t)(i * 29u + 7u);
    }

    send_context.channel = client;
    send_context.payload = payload;
    send_context.payload_size = 1024u * 1024u;
    send_context.entered = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(send_context.entered != NULL, "write-busy entered event");
    if (!send_context.entered) goto cleanup;

    sender_thread = CreateThread(
        NULL,
        0,
        blocked_send_main,
        &send_context,
        0,
        &thread_id);
    CHECK(sender_thread != NULL, "write-busy sender thread");
    if (!sender_thread) goto cleanup;

    wait_result = WaitForSingleObject(send_context.entered, 1000u);
    CHECK(wait_result == WAIT_OBJECT_0, "write-busy sender entered");
    Sleep(20u);

    second_message.kind = VIPC_KIND_NOTIFY;
    second_message.flags = VIPC_FLAG_NONE;
    second_message.operation = VIPC_APP_OPERATION_MIN + 31u;
    second_message.correlation_id = 0u;
    second_message.payload_size = 0u;
    status = vipc_channel_send(client, &second_message, NULL, 50u, &error);
    CHECK(status == VIPC_ERR_BUSY, "concurrent second write rejected");

    wait_result = WaitForSingleObject(sender_thread, 2500u);
    CHECK(wait_result == WAIT_OBJECT_0, "blocked sender completes");
    if (wait_result == WAIT_OBJECT_0) {
        CHECK(send_context.status == VIPC_OK, "blocked first send succeeds");
    }

cleanup:
    if (sender_thread) (void)CloseHandle(sender_thread);
    if (send_context.entered) (void)CloseHandle(send_context.entered);
    free(payload);
    if (client) vipc_channel_destroy(client);

    if (server_thread) {
        wait_result = WaitForSingleObject(server_thread, 2500u);
        CHECK(wait_result == WAIT_OBJECT_0, "write-busy server exits");
        (void)CloseHandle(server_thread);
    }
    CHECK(server_context.failures == 0, "write-busy server assertions");
    vipc_server_destroy(server);
}

static int build_test_pipe_name(
    const char *endpoint,
    WCHAR out_name[256]) {
    DWORD session_id = 0;
    int written;
    size_t used;
    size_t i;

    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) return 0;

    written = swprintf_s(
        out_name,
        256u,
        L"\\\\.\\pipe\\VectorIPC.%lu.",
        (unsigned long)session_id);
    if (written <= 0) return 0;

    used = (size_t)written;
    for (i = 0; endpoint[i] != '\0'; ++i) {
        if (used + 1u >= 256u) return 0;
        out_name[used++] = (WCHAR)(unsigned char)endpoint[i];
    }
    out_name[used] = L'\0';
    return 1;
}

static HANDLE open_raw_pipe(const char *endpoint) {
    WCHAR pipe_name[256];
    uint32_t attempt;

    if (!build_test_pipe_name(endpoint, pipe_name)) return INVALID_HANDLE_VALUE;

    for (attempt = 0; attempt < 500u; ++attempt) {
        HANDLE pipe = CreateFileW(
            pipe_name,
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
            NULL);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;

        if (GetLastError() != ERROR_FILE_NOT_FOUND
            && GetLastError() != ERROR_PIPE_BUSY) {
            return INVALID_HANDLE_VALUE;
        }
        Sleep(1u);
    }

    return INVALID_HANDLE_VALUE;
}

typedef struct raw_receive_context {
    vipc_server *server;
    vipc_status status;
    int failures;
} raw_receive_context;

static DWORD WINAPI raw_receive_main(LPVOID opaque) {
    raw_receive_context *context = (raw_receive_context *)opaque;
    vipc_channel *channel = NULL;
    vipc_error error;
    vipc_message message;
    uint8_t payload[64];
    uint32_t payload_size = 0;
    vipc_status status;

    status = vipc_server_accept(
        context->server,
        2000u,
        &channel,
        &error);
    if (status != VIPC_OK) {
        context->status = status;
        ++context->failures;
        return 1u;
    }

    context->status = vipc_channel_receive(
        channel,
        &message,
        payload,
        (uint32_t)sizeof(payload),
        &payload_size,
        1000u,
        &error);

    vipc_channel_destroy(channel);
    return 0u;
}

static void run_raw_wire_case(
    const char *suffix,
    const uint8_t *bytes,
    uint32_t byte_count,
    vipc_status expected_status) {
    char endpoint[80];
    vipc_server *server = NULL;
    vipc_error error;
    vipc_status status;
    raw_receive_context context;
    HANDLE thread = NULL;
    DWORD thread_id = 0;
    HANDLE raw_pipe = INVALID_HANDLE_VALUE;
    DWORD written = 0;
    DWORD wait_result;

    ZeroMemory(&context, sizeof(context));

    (void)sprintf_s(
        endpoint,
        sizeof(endpoint),
        "raw-%lu-%s",
        (unsigned long)GetCurrentProcessId(),
        suffix);

    status = vipc_server_create(endpoint, &server, &error);
    CHECK(status == VIPC_OK, "raw-wire server create");
    if (status != VIPC_OK) return;

    context.server = server;
    thread = CreateThread(
        NULL,
        0,
        raw_receive_main,
        &context,
        0,
        &thread_id);
    CHECK(thread != NULL, "raw-wire receiver thread");
    if (!thread) goto cleanup;

    raw_pipe = open_raw_pipe(endpoint);
    CHECK(raw_pipe != INVALID_HANDLE_VALUE, "raw-wire open pipe");
    if (raw_pipe == INVALID_HANDLE_VALUE) goto cleanup;

    CHECK(
        WriteFile(raw_pipe, bytes, byte_count, &written, NULL) != FALSE,
        "raw-wire write");
    CHECK(written == byte_count, "raw-wire full write");
    (void)FlushFileBuffers(raw_pipe);
    (void)CloseHandle(raw_pipe);
    raw_pipe = INVALID_HANDLE_VALUE;

    wait_result = WaitForSingleObject(thread, 2000u);
    CHECK(wait_result == WAIT_OBJECT_0, "raw-wire receiver exits");
    if (wait_result == WAIT_OBJECT_0) {
        CHECK(context.status == expected_status, "raw-wire expected status");
    }
    CHECK(context.failures == 0, "raw-wire server assertions");

cleanup:
    if (raw_pipe != NULL && raw_pipe != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(raw_pipe);
    }
    if (thread) (void)CloseHandle(thread);
    vipc_server_destroy(server);
}

static void put_test_u32_le(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value & 0xffu);
    bytes[1] = (uint8_t)((value >> 8) & 0xffu);
    bytes[2] = (uint8_t)((value >> 16) & 0xffu);
    bytes[3] = (uint8_t)((value >> 24) & 0xffu);
}

static void test_malformed_and_truncated_wire(void) {
    vipc_message message;
    uint8_t header[VIPC_WIRE_HEADER_SIZE];
    uint8_t bad[VIPC_WIRE_HEADER_SIZE];
    uint8_t partial_payload[VIPC_WIRE_HEADER_SIZE + 3u];
    vipc_protocol_status protocol_status;

    message.kind = VIPC_KIND_REQUEST;
    message.flags = VIPC_FLAG_NONE;
    message.operation = VIPC_APP_OPERATION_MIN + 40u;
    message.correlation_id = UINT64_C(0x4444);
    message.payload_size = 0u;
    protocol_status = vipc_protocol_encode(&message, header);
    CHECK(protocol_status == VIPC_PROTOCOL_OK, "raw-wire base header encode");
    if (protocol_status != VIPC_PROTOCOL_OK) return;

    memcpy(bad, header, sizeof(bad));
    bad[0] = (uint8_t)'X';
    run_raw_wire_case(
        "magic",
        bad,
        VIPC_WIRE_HEADER_SIZE,
        VIPC_ERR_PROTOCOL_MAGIC);

    memcpy(bad, header, sizeof(bad));
    bad[4] = 2u;
    run_raw_wire_case(
        "version",
        bad,
        VIPC_WIRE_HEADER_SIZE,
        VIPC_ERR_PROTOCOL_VERSION);

    memcpy(bad, header, sizeof(bad));
    put_test_u32_le(
        bad + 28,
        VIPC_MAX_PAYLOAD_BYTES + 1u);
    run_raw_wire_case(
        "oversize",
        bad,
        VIPC_WIRE_HEADER_SIZE,
        VIPC_ERR_PAYLOAD_TOO_LARGE);

    run_raw_wire_case(
        "shortheader",
        header,
        7u,
        VIPC_ERR_PEER_CLOSED);

    message.payload_size = 16u;
    protocol_status = vipc_protocol_encode(&message, header);
    CHECK(protocol_status == VIPC_PROTOCOL_OK, "partial-payload header encode");
    if (protocol_status != VIPC_PROTOCOL_OK) return;
    memcpy(partial_payload, header, VIPC_WIRE_HEADER_SIZE);
    partial_payload[VIPC_WIRE_HEADER_SIZE + 0u] = 1u;
    partial_payload[VIPC_WIRE_HEADER_SIZE + 1u] = 2u;
    partial_payload[VIPC_WIRE_HEADER_SIZE + 2u] = 3u;
    run_raw_wire_case(
        "shortpayload",
        partial_payload,
        (uint32_t)sizeof(partial_payload),
        VIPC_ERR_PEER_CLOSED);
}

typedef struct churn_server_context {
    vipc_server *server;
    uint32_t count;
    int failures;
} churn_server_context;

static DWORD WINAPI churn_server_main(LPVOID opaque) {
    churn_server_context *context = (churn_server_context *)opaque;
    uint32_t i;

    for (i = 0; i < context->count; ++i) {
        vipc_channel *channel = NULL;
        vipc_error error;
        vipc_message request;
        vipc_message response;
        uint8_t payload[1];
        uint32_t payload_size = 0;
        vipc_status status = vipc_server_accept(
            context->server,
            2000u,
            &channel,
            &error);
        if (status != VIPC_OK) {
            ++context->failures;
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
            || request.operation != VIPC_APP_OPERATION_MIN + 50u
            || request.correlation_id != (uint64_t)i + 1u
            || payload_size != 0u) {
            ++context->failures;
            vipc_channel_destroy(channel);
            return 1u;
        }

        response.kind = VIPC_KIND_RESPONSE;
        response.flags = VIPC_FLAG_NONE;
        response.operation = request.operation;
        response.correlation_id = request.correlation_id;
        response.payload_size = 0u;
        status = vipc_channel_send(
            channel,
            &response,
            NULL,
            2000u,
            &error);
        if (status != VIPC_OK) {
            ++context->failures;
            vipc_channel_destroy(channel);
            return 1u;
        }

        vipc_channel_destroy(channel);
    }
    return 0u;
}

static void test_connection_churn(void) {
    enum { CHURN_COUNT = 64 };
    char endpoint[64];
    vipc_server *server = NULL;
    vipc_error error;
    vipc_status status;
    churn_server_context context;
    HANDLE thread = NULL;
    DWORD thread_id = 0;
    DWORD wait_result;
    uint32_t i;

    ZeroMemory(&context, sizeof(context));
    (void)sprintf_s(
        endpoint,
        sizeof(endpoint),
        "churn-%lu",
        (unsigned long)GetCurrentProcessId());

    status = vipc_server_create(endpoint, &server, &error);
    CHECK(status == VIPC_OK, "churn server create");
    if (status != VIPC_OK) return;

    context.server = server;
    context.count = CHURN_COUNT;
    thread = CreateThread(
        NULL,
        0,
        churn_server_main,
        &context,
        0,
        &thread_id);
    CHECK(thread != NULL, "churn server thread");
    if (!thread) {
        vipc_server_destroy(server);
        return;
    }

    for (i = 0; i < CHURN_COUNT; ++i) {
        vipc_channel *client = NULL;
        vipc_message request;
        vipc_message response;
        uint8_t payload[1];
        uint32_t payload_size = 0;

        status = vipc_client_connect(endpoint, 2000u, &client, &error);
        CHECK(status == VIPC_OK, "churn client connect");
        if (status != VIPC_OK) break;

        request.kind = VIPC_KIND_REQUEST;
        request.flags = VIPC_FLAG_NONE;
        request.operation = VIPC_APP_OPERATION_MIN + 50u;
        request.correlation_id = (uint64_t)i + 1u;
        request.payload_size = 0u;
        status = vipc_channel_send(
            client,
            &request,
            NULL,
            2000u,
            &error);
        CHECK(status == VIPC_OK, "churn request send");
        if (status != VIPC_OK) {
            vipc_channel_destroy(client);
            break;
        }

        status = vipc_channel_receive(
            client,
            &response,
            payload,
            (uint32_t)sizeof(payload),
            &payload_size,
            2000u,
            &error);
        CHECK(status == VIPC_OK, "churn response receive");
        CHECK(
            status != VIPC_OK
                || (response.kind == VIPC_KIND_RESPONSE
                    && response.operation == request.operation
                    && response.correlation_id == request.correlation_id
                    && payload_size == 0u),
            "churn response identity");

        vipc_channel_destroy(client);
        if (status != VIPC_OK) break;
    }

    wait_result = WaitForSingleObject(thread, 3000u);
    CHECK(wait_result == WAIT_OBJECT_0, "churn server exits");
    CHECK(context.failures == 0, "churn server assertions");
    (void)CloseHandle(thread);
    vipc_server_destroy(server);
}

static void test_preaccept_disconnect_recovery(void) {
    enum { RECOVERY_CYCLES = 32 };
    char endpoint[64];
    vipc_server *server = NULL;
    vipc_error error;
    vipc_status status;
    DWORD handles_before = 0u;
    DWORD handles_after = 0u;
    uint32_t i;

    (void)sprintf_s(
        endpoint,
        sizeof(endpoint),
        "preaccept-recovery-%lu",
        (unsigned long)GetCurrentProcessId());

    status = vipc_server_create(endpoint, &server, &error);
    CHECK(status == VIPC_OK, "preaccept recovery server create");
    if (status != VIPC_OK) return;

    CHECK(
        GetProcessHandleCount(GetCurrentProcess(), &handles_before) != FALSE,
        "preaccept recovery handle baseline");

    for (i = 0u; i < RECOVERY_CYCLES; ++i) {
        vipc_channel *ghost = NULL;
        vipc_channel *stale = NULL;
        vipc_channel *client = NULL;
        vipc_channel *accepted = NULL;

        status = vipc_client_connect(endpoint, 1000u, &ghost, &error);
        CHECK(status == VIPC_OK, "preaccept ghost client connect");
        if (status != VIPC_OK) break;

        vipc_channel_destroy(ghost);
        Sleep(1u);

        status = vipc_server_accept(server, 25u, &stale, &error);
        CHECK(
            status == VIPC_OK
                || status == VIPC_ERR_PEER_CLOSED
                || status == VIPC_ERR_TIMEOUT,
            "preaccept disconnected client is recoverable");
        if (status == VIPC_OK) {
            vipc_channel_destroy(stale);
        } else {
            CHECK(stale == NULL, "failed preaccept does not publish channel");
        }

        status = vipc_client_connect(endpoint, 1000u, &client, &error);
        CHECK(status == VIPC_OK, "post-recovery client connect");
        if (status != VIPC_OK) break;

        status = vipc_server_accept(server, 1000u, &accepted, &error);
        CHECK(status == VIPC_OK, "post-recovery server accept");
        CHECK(
            status != VIPC_OK
                || (accepted != NULL
                    && vipc_channel_peer_pid(accepted) != 0u
                    && vipc_channel_peer_session_id(accepted) != 0u),
            "post-recovery accepted peer identity");

        vipc_channel_destroy(client);
        if (accepted) vipc_channel_destroy(accepted);
        if (status != VIPC_OK) break;
    }

    CHECK(
        GetProcessHandleCount(GetCurrentProcess(), &handles_after) != FALSE,
        "preaccept recovery handle final");
    CHECK(
        handles_before == handles_after,
        "preaccept recovery handle count stable");

    vipc_server_destroy(server);
}

#endif

static void trace_stage(const char *name) {
    int enabled = 0;
#ifdef _WIN32
    {
        char value[2];
        enabled = GetEnvironmentVariableA(
            "VIPC_SELFTEST_TRACE",
            value,
            (DWORD)sizeof(value)) != 0u;
    }
#else
    enabled = getenv("VIPC_SELFTEST_TRACE") != NULL;
#endif
    if (enabled) {
        fprintf(stderr, "SELFTEST_STAGE:%s\n", name);
        fflush(stderr);
    }
}

int main(void) {
    trace_stage("abi:start");
    test_abi_contract();
    trace_stage("abi:done");
    trace_stage("protocol:start");
    test_protocol();
    trace_stage("protocol:done");
    trace_stage("endpoint:start");
    test_endpoint_validation();
    trace_stage("endpoint:done");

#ifdef _WIN32
    trace_stage("windows_transport:start");
    test_windows_transport();
    trace_stage("windows_transport:done");
    trace_stage("concurrent_write_busy:start");
    test_concurrent_write_busy();
    trace_stage("concurrent_write_busy:done");
    trace_stage("malformed_wire:start");
    test_malformed_and_truncated_wire();
    trace_stage("malformed_wire:done");
    trace_stage("connection_churn:start");
    test_connection_churn();
    trace_stage("connection_churn:done");
    trace_stage("preaccept_disconnect_recovery:start");
    test_preaccept_disconnect_recovery();
    trace_stage("preaccept_disconnect_recovery:done");
#else
    printf("VectorIPC: platform transport tests skipped on this host\n");
#endif

    if (g_failures != 0) {
        fprintf(stderr, "VectorIPC selftest: %d failure(s)\n", g_failures);
        return 1;
    }

    printf("VectorIPC selftest: PASS\n");
    return 0;
}