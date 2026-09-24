#include "vectoripc/vipc.h"
#include "vipc_externalobject_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

typedef struct echo_server_context {
    vipc_server *server;
    uint32_t expected_operation;
    const uint8_t *expected_payload;
    uint32_t expected_payload_size;
    const uint8_t *reply_payload;
    uint32_t reply_payload_size;
    uint32_t request_count;
    int failed;
} echo_server_context;

static vipc_es_tagged_data number_arg(double value) {
    vipc_es_tagged_data result;
    ZeroMemory(&result, sizeof(result));
    result.type = VIPC_ES_TYPE_DOUBLE;
    result.data.fltval = value;
    return result;
}

static double pack6(const uint8_t *bytes, uint32_t count) {
    uint64_t value = 0u;
    uint32_t i;
    for (i = 0u; i < count; ++i) {
        value |= (uint64_t)bytes[i] << (i * 8u);
    }
    return (double)value;
}

static int call_text(
    vipc_es_tagged_data *args,
    long argc,
    char *out,
    size_t out_capacity) {
    vipc_es_tagged_data retval;
    long status;
    size_t length;

    ZeroMemory(&retval, sizeof(retval));
    status = vipc(args, argc, &retval);
    if (status != VIPC_ES_ERR_OK
        || retval.type != VIPC_ES_TYPE_STRING
        || !retval.data.string) {
        return 0;
    }

    length = strlen(retval.data.string);
    if (length + 1u > out_capacity) {
        ESFreeMem(retval.data.string);
        return 0;
    }

    memcpy(out, retval.data.string, length + 1u);
    ESFreeMem(retval.data.string);
    return 1;
}

static int parse_connected_handle(const char *text, uint32_t *out_handle) {
    static const char prefix[] = "VIPC/1.0|OK|CONNECTED|";
    const char *cursor;
    char *end = NULL;
    unsigned long value;

    if (!text || !out_handle) return 0;
    if (strncmp(text, prefix, sizeof(prefix) - 1u) != 0) return 0;

    cursor = text + sizeof(prefix) - 1u;
    value = strtoul(cursor, &end, 10);
    if (!end
        || end == cursor
        || *end != '|'
        || value == 0u
        || value > UINT32_MAX) {
        return 0;
    }

    *out_handle = (uint32_t)value;
    return 1;
}

static int connect_adapter_text(
    const char *endpoint,
    char *result,
    size_t result_capacity) {
    vipc_es_tagged_data args[32];
    uint8_t endpoint_bytes[80];
    uint32_t endpoint_size = (uint32_t)strlen(endpoint);
    uint32_t packed_count = (endpoint_size + 5u) / 6u;
    uint32_t i;

    if (endpoint_size == 0u || endpoint_size > sizeof(endpoint_bytes)) return 0;
    memcpy(endpoint_bytes, endpoint, endpoint_size);

    args[0] = number_arg(1.0);
    args[1] = number_arg(2000.0);
    args[2] = number_arg((double)endpoint_size);
    for (i = 0u; i < packed_count; ++i) {
        uint32_t offset = i * 6u;
        uint32_t count = endpoint_size - offset;
        if (count > 6u) count = 6u;
        args[3u + i] = number_arg(pack6(endpoint_bytes + offset, count));
    }

    return call_text(
        args,
        (long)(3u + packed_count),
        result,
        result_capacity);
}

static int connect_adapter(
    const char *endpoint,
    uint32_t *out_handle) {
    char result[256];
    return connect_adapter_text(endpoint, result, sizeof(result))
        && parse_connected_handle(result, out_handle);
}

static int stage_payload(
    uint32_t handle,
    const uint8_t *payload,
    uint32_t payload_size) {
    vipc_es_tagged_data args[64];
    uint32_t packed_count = (payload_size + 5u) / 6u;
    uint32_t i;
    char result[128];

    args[0] = number_arg(2.0);
    args[1] = number_arg((double)handle);
    if (!call_text(args, 2, result, sizeof(result))
        || strcmp(result, "VIPC/1.0|OK|STAGE_RESET") != 0) {
        return 0;
    }

    args[0] = number_arg(3.0);
    args[1] = number_arg((double)handle);
    args[2] = number_arg((double)payload_size);
    for (i = 0u; i < packed_count; ++i) {
        uint32_t offset = i * 6u;
        uint32_t count = payload_size - offset;
        if (count > 6u) count = 6u;
        args[3u + i] = number_arg(pack6(payload + offset, count));
    }

    return call_text(args, (long)(3u + packed_count), result, sizeof(result))
        && strncmp(result, "VIPC/1.0|OK|STAGED|", 16u) == 0;
}

static int transact_adapter(
    uint32_t handle,
    uint32_t operation,
    uint32_t correlation,
    const char *expected_base64) {
    vipc_es_tagged_data args[6];
    char result[256];
    char expected[256];

    args[0] = number_arg(4.0);
    args[1] = number_arg((double)handle);
    args[2] = number_arg(2000.0);
    args[3] = number_arg((double)operation);
    args[4] = number_arg((double)correlation);
    args[5] = number_arg(0.0);

    if (!call_text(args, 6, result, sizeof(result))) return 0;

    (void)sprintf_s(
        expected,
        sizeof(expected),
        "VIPC/1.0|FRAME|2|0|%lu|%lu|0|3|%s",
        (unsigned long)operation,
        (unsigned long)correlation,
        expected_base64);
    return strcmp(result, expected) == 0;
}

static int close_adapter(uint32_t handle) {
    vipc_es_tagged_data args[2];
    char result[128];

    args[0] = number_arg(7.0);
    args[1] = number_arg((double)handle);
    return call_text(args, 2, result, sizeof(result))
        && strcmp(result, "VIPC/1.0|OK|CLOSED") == 0;
}

static DWORD WINAPI echo_server_main(LPVOID opaque) {
    echo_server_context *context = (echo_server_context *)opaque;
    vipc_channel *channel = NULL;
    vipc_error error;
    uint32_t request_index;
    vipc_status status;

    status = vipc_server_accept(
        context->server,
        3000u,
        &channel,
        &error);
    if (status != VIPC_OK) {
        context->failed = 1;
        return 1u;
    }

    for (request_index = 0u;
         request_index < context->request_count;
         ++request_index) {
        vipc_message request;
        vipc_message response;
        uint8_t payload[64];
        uint32_t payload_size = 0u;

        status = vipc_channel_receive(
            channel,
            &request,
            payload,
            (uint32_t)sizeof(payload),
            &payload_size,
            3000u,
            &error);
        if (status != VIPC_OK
            || request.kind != VIPC_KIND_REQUEST
            || request.operation != context->expected_operation
            || payload_size != context->expected_payload_size
            || memcmp(
                payload,
                context->expected_payload,
                context->expected_payload_size) != 0) {
            context->failed = 1;
            break;
        }

        response.kind = VIPC_KIND_RESPONSE;
        response.flags = VIPC_FLAG_NONE;
        response.operation = request.operation;
        response.correlation_id = request.correlation_id;
        response.payload_size = context->reply_payload_size;

        status = vipc_channel_send(
            channel,
            &response,
            context->reply_payload,
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

static int test_session_limit_and_generation(void) {
    enum { SESSION_COUNT = 16 };
    vipc_server *servers[SESSION_COUNT + 1u];
    uint32_t handles[SESSION_COUNT];
    char endpoints[SESSION_COUNT + 1u][64];
    char result[256];
    vipc_error error;
    vipc_status status;
    uint32_t replacement_handle = 0u;
    uint32_t stale_handle;
    uint32_t i;
    int failed = 0;

    ZeroMemory(servers, sizeof(servers));
    ZeroMemory(handles, sizeof(handles));

    for (i = 0u; i < SESSION_COUNT; ++i) {
        (void)sprintf_s(
            endpoints[i],
            sizeof(endpoints[i]),
            "eo-limit-%lu-%lu",
            (unsigned long)GetCurrentProcessId(),
            (unsigned long)i);

        status = vipc_server_create(endpoints[i], &servers[i], &error);
        if (status != VIPC_OK) {
            fprintf(
                stderr,
                "session-limit server %lu create failed: %s\n",
                (unsigned long)i,
                vipc_status_name(status));
            failed = 1;
            goto cleanup;
        }

        if (!connect_adapter(endpoints[i], &handles[i])) {
            fprintf(stderr, "session-limit connect %lu failed\n",
                (unsigned long)i);
            failed = 1;
            goto cleanup;
        }
    }

    if (!connect_adapter_text(endpoints[0], result, sizeof(result))
        || strcmp(result, "VIPC/1.0|ERR|STATE|SESSION_LIMIT") != 0) {
        fprintf(stderr, "17th session was not rejected: %s\n", result);
        failed = 1;
        goto cleanup;
    }

    stale_handle = handles[0];
    if (!close_adapter(stale_handle)) {
        fprintf(stderr, "session-limit first close failed\n");
        failed = 1;
        goto cleanup;
    }
    handles[0] = 0u;

    (void)sprintf_s(
        endpoints[SESSION_COUNT],
        sizeof(endpoints[SESSION_COUNT]),
        "eo-limit-%lu-r",
        (unsigned long)GetCurrentProcessId());
    status = vipc_server_create(
        endpoints[SESSION_COUNT],
        &servers[SESSION_COUNT],
        &error);
    if (status != VIPC_OK) {
        fprintf(stderr, "replacement server create failed: %s\n",
            vipc_status_name(status));
        failed = 1;
        goto cleanup;
    }

    if (!connect_adapter(
            endpoints[SESSION_COUNT],
            &replacement_handle)) {
        fprintf(stderr, "replacement session connect failed\n");
        failed = 1;
        goto cleanup;
    }

    if (replacement_handle == stale_handle) {
        fprintf(stderr, "replacement reused stale generation token\n");
        failed = 1;
        goto cleanup;
    }

    if (!close_adapter(replacement_handle)) {
        fprintf(stderr, "replacement session close failed\n");
        failed = 1;
        goto cleanup;
    }
    replacement_handle = 0u;

cleanup:
    if (replacement_handle != 0u) {
        (void)close_adapter(replacement_handle);
    }
    for (i = 0u; i < SESSION_COUNT; ++i) {
        if (handles[i] != 0u) {
            (void)close_adapter(handles[i]);
        }
    }
    for (i = 0u; i < SESSION_COUNT + 1u; ++i) {
        if (servers[i]) {
            vipc_server_destroy(servers[i]);
        }
    }
    ESTerminate();
    return failed ? 0 : 1;
}

int main(void) {
    char endpoint_a[64];
    char endpoint_b[64];
    vipc_server *server_a = NULL;
    vipc_server *server_b = NULL;
    vipc_error error;
    vipc_status status;
    echo_server_context context_a;
    echo_server_context context_b;
    HANDLE thread_a = NULL;
    HANDLE thread_b = NULL;
    DWORD thread_id = 0u;
    DWORD wait_result;
    uint32_t handle_a = 0u;
    uint32_t handle_b = 0u;
    int failed = 0;
    vipc_es_tagged_data stale_args[2];
    char stale_result[128];
    static const uint8_t payload_a[] = { 0x00u, 0xa1u, 0xa2u, 0xffu };
    static const uint8_t payload_b[] = { 0x00u, 0xb1u, 0xb2u, 0x80u, 0x7fu };
    static const uint8_t reply_a[] = { 'A', 'A', 'A' };
    static const uint8_t reply_b[] = { 'B', 'B', 'B' };

    ZeroMemory(&context_a, sizeof(context_a));
    ZeroMemory(&context_b, sizeof(context_b));

    (void)sprintf_s(
        endpoint_a,
        sizeof(endpoint_a),
        "eo-session-a-%lu",
        (unsigned long)GetCurrentProcessId());
    (void)sprintf_s(
        endpoint_b,
        sizeof(endpoint_b),
        "eo-session-b-%lu",
        (unsigned long)GetCurrentProcessId());

    status = vipc_server_create(endpoint_a, &server_a, &error);
    if (status != VIPC_OK) {
        fprintf(stderr, "server A create failed: %s\n", vipc_status_name(status));
        return 1;
    }
    status = vipc_server_create(endpoint_b, &server_b, &error);
    if (status != VIPC_OK) {
        fprintf(stderr, "server B create failed: %s\n", vipc_status_name(status));
        vipc_server_destroy(server_a);
        return 1;
    }

    context_a.server = server_a;
    context_a.expected_operation = VIPC_APP_OPERATION_MIN + 80u;
    context_a.expected_payload = payload_a;
    context_a.expected_payload_size = (uint32_t)sizeof(payload_a);
    context_a.reply_payload = reply_a;
    context_a.reply_payload_size = (uint32_t)sizeof(reply_a);
    context_a.request_count = 1u;

    context_b.server = server_b;
    context_b.expected_operation = VIPC_APP_OPERATION_MIN + 81u;
    context_b.expected_payload = payload_b;
    context_b.expected_payload_size = (uint32_t)sizeof(payload_b);
    context_b.reply_payload = reply_b;
    context_b.reply_payload_size = (uint32_t)sizeof(reply_b);
    context_b.request_count = 2u;

    thread_a = CreateThread(
        NULL, 0, echo_server_main, &context_a, 0, &thread_id);
    thread_b = CreateThread(
        NULL, 0, echo_server_main, &context_b, 0, &thread_id);
    if (!thread_a || !thread_b) {
        fprintf(stderr, "server thread creation failed\n");
        failed = 1;
        goto cleanup;
    }

    if (!connect_adapter(endpoint_a, &handle_a)
        || !connect_adapter(endpoint_b, &handle_b)
        || handle_a == handle_b) {
        fprintf(stderr, "independent CONNECT handles failed\n");
        failed = 1;
        goto cleanup;
    }

    if (!stage_payload(handle_a, payload_a, (uint32_t)sizeof(payload_a))
        || !stage_payload(handle_b, payload_b, (uint32_t)sizeof(payload_b))) {
        fprintf(stderr, "independent staging failed\n");
        failed = 1;
        goto cleanup;
    }

    /* Deliberately service B first after staging A then B. */
    if (!transact_adapter(
            handle_b,
            VIPC_APP_OPERATION_MIN + 81u,
            0x2001u,
            "QkJC")) {
        fprintf(stderr, "session B first transaction failed\n");
        failed = 1;
        goto cleanup;
    }

    if (!transact_adapter(
            handle_a,
            VIPC_APP_OPERATION_MIN + 80u,
            0x1001u,
            "QUFB")) {
        fprintf(stderr, "session A transaction failed\n");
        failed = 1;
        goto cleanup;
    }

    if (!close_adapter(handle_a)) {
        fprintf(stderr, "session A close failed\n");
        failed = 1;
        goto cleanup;
    }

    stale_args[0] = number_arg(2.0);
    stale_args[1] = number_arg((double)handle_a);
    if (!call_text(stale_args, 2, stale_result, sizeof(stale_result))
        || strcmp(stale_result, "VIPC/1.0|ERR|STATE|INVALID_HANDLE") != 0) {
        fprintf(stderr, "session A stale handle was accepted\n");
        failed = 1;
        goto cleanup;
    }

    /* Closing A must not alter B's staged payload or live channel. */
    if (!transact_adapter(
            handle_b,
            VIPC_APP_OPERATION_MIN + 81u,
            0x2002u,
            "QkJC")) {
        fprintf(stderr, "session B did not survive session A close\n");
        failed = 1;
        goto cleanup;
    }

    if (!close_adapter(handle_b)) {
        fprintf(stderr, "session B close failed\n");
        failed = 1;
        goto cleanup;
    }
    handle_b = 0u;

cleanup:
    if (handle_a != 0u) {
        (void)close_adapter(handle_a);
    }
    if (handle_b != 0u) {
        (void)close_adapter(handle_b);
    }

    if (thread_a) {
        wait_result = WaitForSingleObject(thread_a, 3500u);
        if (wait_result != WAIT_OBJECT_0) failed = 1;
        (void)CloseHandle(thread_a);
    }
    if (thread_b) {
        wait_result = WaitForSingleObject(thread_b, 3500u);
        if (wait_result != WAIT_OBJECT_0) failed = 1;
        (void)CloseHandle(thread_b);
    }

    if (context_a.failed || context_b.failed) failed = 1;

    vipc_server_destroy(server_a);
    vipc_server_destroy(server_b);
    ESTerminate();

    if (!failed && !test_session_limit_and_generation()) {
        failed = 1;
    }

    if (failed) return 1;
    printf("VectorIPC ExternalObject multi-session isolation: PASS\n");
    return 0;
}

#else

int main(void) {
    printf("VectorIPC ExternalObject sessions: SKIP (Windows only)\n");
    return 0;
}

#endif