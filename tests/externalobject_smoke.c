#include "vectoripc/vipc.h"
#include "vipc_externalobject_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

typedef esabi_error (ESABI_CALL *vipc_method_fn)(
    esabi_value *,
    esabi_long,
    esabi_value *);
typedef char *(ESABI_CALL *es_initialize_fn)(esabi_value *, esabi_long);
typedef esabi_long (ESABI_CALL *es_get_version_fn)(void);
typedef void (*es_free_mem_fn)(void *);
typedef void (*es_terminate_fn)(void);

typedef struct eo_exports {
    HMODULE module;
    vipc_method_fn method;
    es_initialize_fn initialize;
    es_get_version_fn get_version;
    es_free_mem_fn free_mem;
    es_terminate_fn terminate;
} eo_exports;

typedef struct server_context {
    vipc_server *server;
    int failed;
} server_context;

static int load_exports(const char *path, eo_exports *out) {
    ZeroMemory(out, sizeof(*out));
    out->module = LoadLibraryA(path);
    if (!out->module) return 0;

    out->method = (vipc_method_fn)(void *)GetProcAddress(out->module, "vipc");
    out->initialize = (es_initialize_fn)(void *)GetProcAddress(
        out->module,
        "ESInitialize");
    out->get_version = (es_get_version_fn)(void *)GetProcAddress(
        out->module,
        "ESGetVersion");
    out->free_mem = (es_free_mem_fn)(void *)GetProcAddress(
        out->module,
        "ESFreeMem");
    out->terminate = (es_terminate_fn)(void *)GetProcAddress(
        out->module,
        "ESTerminate");

    return out->method
        && out->initialize
        && out->get_version
        && out->free_mem
        && out->terminate;
}

static void unload_exports(eo_exports *exports) {
    if (exports->module) (void)FreeLibrary(exports->module);
    ZeroMemory(exports, sizeof(*exports));
}

static esabi_value number_arg(double value) {
    esabi_value result;
    ZeroMemory(&result, sizeof(result));
    result.type = ESABI_TYPE_DOUBLE;
    result.payload.double_value = value;
    return result;
}

static esabi_value string_arg(char *value) {
    esabi_value result;
    ZeroMemory(&result, sizeof(result));
    result.type = ESABI_TYPE_STRING;
    result.payload.string_value = value;
    return result;
}

static double pack6(const uint8_t *bytes, uint32_t count) {
    uint64_t value = 0u;
    uint32_t i;
    for (i = 0; i < count; ++i) {
        value |= (uint64_t)bytes[i] << (i * 8u);
    }
    return (double)value;
}

static int call_string(
    eo_exports *exports,
    esabi_value *args,
    long argc,
    const char **out_text,
    esabi_value *out_retval) {
    long es_status;
    ZeroMemory(out_retval, sizeof(*out_retval));

    es_status = exports->method(args, argc, out_retval);
    if (es_status != ESABI_OK
        || out_retval->type != ESABI_TYPE_STRING
        || !out_retval->payload.string_value) {
        return 0;
    }
    *out_text = out_retval->payload.string_value;
    return 1;
}

static void free_result(
    eo_exports *exports,
    esabi_value *retval) {
    if (retval->type == ESABI_TYPE_STRING && retval->payload.string_value) {
        exports->free_mem(retval->payload.string_value);
    }
    ZeroMemory(retval, sizeof(*retval));
}

static int parse_connected_handle(
    const char *text,
    uint32_t *out_handle) {
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

static DWORD WINAPI server_main(LPVOID opaque) {
    server_context *context = (server_context *)opaque;
    vipc_channel *channel = NULL;
    vipc_error error;
    vipc_status status;
    vipc_message request;
    vipc_message response;
    uint8_t payload[64];
    uint32_t payload_size = 0;
    static const uint8_t expected[] = {
        0x00u, 0x01u, 0x02u, 0xffu, 0x10u, 0x80u, 0x7fu
    };
    static const uint8_t reply[] = { 0x00u, 0x01u, 0x02u, 0xffu };
    static const uint8_t expected_text[] = {
        'h', 0xc3u, 0xa9u, 'l', 'l', 'o', '|', 'w', 'o', 'r', 'l', 'd'
    };
    static const uint8_t text_reply[] = {
        'r', 0xc3u, 0xa9u, 'p', 'l', 'y', '|', 'o', 'k'
    };
    static const uint8_t expected_binary_probe[] = {
        'b', 'i', 'n', 'a', 'r', 'y', '-', 't', 'e', 's', 't'
    };
    static const uint8_t invalid_text_reply[] = { 'x', 0x00u, 'y' };

    status = vipc_server_accept(
        context->server,
        3000u,
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
        3000u,
        &error);
    if (status != VIPC_OK
        || request.kind != VIPC_KIND_REQUEST
        || request.operation != VIPC_APP_OPERATION_MIN + 70u
        || request.correlation_id != UINT64_C(0x12345678abcdef01)
        || payload_size != (uint32_t)sizeof(expected)
        || memcmp(payload, expected, sizeof(expected)) != 0) {
        context->failed = 1;
        vipc_channel_destroy(channel);
        return 1u;
    }

    response.kind = VIPC_KIND_RESPONSE;
    response.flags = VIPC_FLAG_NONE;
    response.operation = request.operation;
    response.correlation_id = request.correlation_id;
    response.payload_size = (uint32_t)sizeof(reply);

    status = vipc_channel_send(
        channel,
        &response,
        reply,
        3000u,
        &error);
    if (status != VIPC_OK) context->failed = 1;

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
        || request.operation != VIPC_APP_OPERATION_MIN + 71u
        || request.correlation_id
            != ((uint64_t)123u | ((uint64_t)456u << 32))
        || payload_size != (uint32_t)sizeof(expected_text)
        || memcmp(payload, expected_text, sizeof(expected_text)) != 0) {
        context->failed = 1;
        vipc_channel_destroy(channel);
        return 1u;
    }

    response.kind = VIPC_KIND_RESPONSE;
    response.flags = VIPC_FLAG_NONE;
    response.operation = request.operation;
    response.correlation_id = request.correlation_id;
    response.payload_size = (uint32_t)sizeof(text_reply);
    status = vipc_channel_send(
        channel,
        &response,
        text_reply,
        3000u,
        &error);
    if (status != VIPC_OK) {
        context->failed = 1;
        vipc_channel_destroy(channel);
        return 1u;
    }

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
        || request.operation != VIPC_APP_OPERATION_MIN + 72u
        || request.correlation_id
            != ((uint64_t)124u | ((uint64_t)456u << 32))
        || payload_size != (uint32_t)sizeof(expected_binary_probe)
        || memcmp(
            payload,
            expected_binary_probe,
            sizeof(expected_binary_probe)) != 0) {
        context->failed = 1;
        vipc_channel_destroy(channel);
        return 1u;
    }

    response.kind = VIPC_KIND_RESPONSE;
    response.flags = VIPC_FLAG_NONE;
    response.operation = request.operation;
    response.correlation_id = request.correlation_id;
    response.payload_size = (uint32_t)sizeof(invalid_text_reply);
    status = vipc_channel_send(
        channel,
        &response,
        invalid_text_reply,
        3000u,
        &error);
    if (status != VIPC_OK) context->failed = 1;

    vipc_channel_destroy(channel);
    return context->failed ? 1u : 0u;
}

static int run_test(const char *dll_path) {
    eo_exports exports;
    char endpoint[64];
    vipc_server *server = NULL;
    vipc_error error;
    vipc_status status;
    server_context server_context;
    HANDLE server_thread = NULL;
    DWORD thread_id = 0;
    DWORD wait_result;
    int failed = 0;
    const char *text = NULL;
    esabi_value retval;
    esabi_value args[32];
    uint8_t endpoint_bytes[64];
    uint32_t endpoint_size;
    uint32_t endpoint_packs;
    uint32_t session_handle = 0u;
    uint32_t i;
    static const uint8_t request_payload[] = {
        0x00u, 0x01u, 0x02u, 0xffu, 0x10u, 0x80u, 0x7fu
    };
    static char request_text[] = "h\xc3\xa9llo|world";
    static char invalid_utf8_request[] = "\xc0\xaf";
    static char binary_probe_text[] = {
        'b', 'i', 'n', 'a', 'r', 'y', '-', 't', 'e', 's', 't', '\0'
    };

    ZeroMemory(&server_context, sizeof(server_context));
    ZeroMemory(&retval, sizeof(retval));

    if (!load_exports(dll_path, &exports)) {
        fprintf(stderr, "failed to load adapter exports: %s (%lu)\n",
            dll_path, (unsigned long)GetLastError());
        return 1;
    }

    if (strcmp(exports.initialize(NULL, 0), "vipc") != 0) {
        fprintf(stderr, "ESInitialize surface mismatch\n");
        failed = 1;
        goto cleanup_exports;
    }
    if (exports.get_version() != 3) {
        fprintf(stderr, "ESGetVersion mismatch\n");
        failed = 1;
        goto cleanup_exports;
    }

    args[0] = number_arg(0.0);
    if (!call_string(&exports, args, 1, &text, &retval)
        || strcmp(text, "VIPC/1.0|INFO|3|1|0|262144|6|16") != 0) {
        fprintf(stderr, "INFO failed: %s\n", text ? text : "<null>");
        failed = 1;
        goto cleanup_exports;
    }
    free_result(&exports, &retval);
    text = NULL;

    (void)sprintf_s(
        endpoint,
        sizeof(endpoint),
        "eo-smoke-%lu",
        (unsigned long)GetCurrentProcessId());
    endpoint_size = (uint32_t)strlen(endpoint);
    memcpy(endpoint_bytes, endpoint, endpoint_size);

    status = vipc_server_create(endpoint, &server, &error);
    if (status != VIPC_OK) {
        fprintf(stderr, "server create: %s\n", vipc_status_name(status));
        failed = 1;
        goto cleanup_exports;
    }

    server_context.server = server;
    server_thread = CreateThread(
        NULL,
        0,
        server_main,
        &server_context,
        0,
        &thread_id);
    if (!server_thread) {
        fprintf(stderr, "server thread create failed\n");
        failed = 1;
        goto cleanup_server;
    }

    endpoint_packs = (endpoint_size + 5u) / 6u;
    args[0] = number_arg(1.0);
    args[1] = number_arg(2000.0);
    args[2] = number_arg((double)endpoint_size);
    for (i = 0; i < endpoint_packs; ++i) {
        uint32_t offset = i * 6u;
        uint32_t count = endpoint_size - offset;
        if (count > 6u) count = 6u;
        args[3u + i] = number_arg(pack6(endpoint_bytes + offset, count));
    }

    if (!call_string(
            &exports,
            args,
            (long)(3u + endpoint_packs),
            &text,
            &retval)
        || !parse_connected_handle(text, &session_handle)) {
        fprintf(stderr, "CONNECT failed: %s\n", text ? text : "<null>");
        failed = 1;
        goto cleanup_thread;
    }
    free_result(&exports, &retval);
    text = NULL;

    args[0] = number_arg(2.0);
    args[1] = number_arg((double)session_handle);
    if (!call_string(&exports, args, 2, &text, &retval)
        || strcmp(text, "VIPC/1.0|OK|STAGE_RESET") != 0) {
        fprintf(stderr, "STAGE_RESET failed: %s\n", text ? text : "<null>");
        failed = 1;
        goto cleanup_thread;
    }
    free_result(&exports, &retval);
    text = NULL;

    /* Zero-byte append is a valid no-op and must not form NULL + 0 internally. */
    args[0] = number_arg(3.0);
    args[1] = number_arg((double)session_handle);
    args[2] = number_arg(0.0);
    if (!call_string(&exports, args, 3, &text, &retval)
        || strcmp(text, "VIPC/1.0|OK|STAGED|0") != 0) {
        fprintf(stderr, "zero STAGE_APPEND failed: %s\n", text ? text : "<null>");
        failed = 1;
        goto cleanup_thread;
    }
    free_result(&exports, &retval);
    text = NULL;

    /* A hostile maximal count must hit the stage limit without count overflow. */
    args[2] = number_arg(4294967295.0);
    if (!call_string(&exports, args, 3, &text, &retval)
        || strcmp(text, "VIPC/1.0|ERR|LIMIT|STAGE") != 0) {
        fprintf(stderr,
            "oversize STAGE_APPEND failed: %s\n",
            text ? text : "<null>");
        failed = 1;
        goto cleanup_thread;
    }
    free_result(&exports, &retval);
    text = NULL;

    args[0] = number_arg(3.0);
    args[1] = number_arg((double)session_handle);
    args[2] = number_arg((double)sizeof(request_payload));
    args[3] = number_arg(pack6(request_payload, 6u));
    args[4] = number_arg(pack6(request_payload + 6u, 1u));
    if (!call_string(&exports, args, 5, &text, &retval)
        || strcmp(text, "VIPC/1.0|OK|STAGED|7") != 0) {
        fprintf(stderr, "STAGE_APPEND failed: %s\n", text ? text : "<null>");
        failed = 1;
        goto cleanup_thread;
    }
    free_result(&exports, &retval);
    text = NULL;

    args[0] = number_arg(4.0);
    args[1] = number_arg((double)session_handle);
    args[2] = number_arg(2000.0);
    args[3] = number_arg((double)(VIPC_APP_OPERATION_MIN + 70u));
    args[4] = number_arg(2882400001.0); /* 0xabcdef01 */
    args[5] = number_arg(305419896.0);  /* 0x12345678 */
    if (!call_string(&exports, args, 6, &text, &retval)
        || strcmp(
            text,
            "VIPC/1.0|FRAME|2|0|326|2882400001|305419896|4|AAEC/w==") != 0) {
        fprintf(stderr, "TRANSACT failed: %s\n", text ? text : "<null>");
        failed = 1;
        goto cleanup_thread;
    }
    free_result(&exports, &retval);
    text = NULL;

    args[0] = number_arg(8.0);
    args[1] = number_arg((double)session_handle);
    args[2] = number_arg(2000.0);
    args[3] = number_arg((double)(VIPC_APP_OPERATION_MIN + 71u));
    args[4] = number_arg(123.0);
    args[5] = number_arg(456.0);
    args[6] = string_arg(request_text);
    if (!call_string(&exports, args, 7, &text, &retval)
        || strcmp(
            text,
            "VIPC/1.0|TEXT|2|0|327|123|456|9|r\xc3\xa9ply|ok") != 0) {
        fprintf(stderr, "TRANSACT_TEXT failed: %s\n", text ? text : "<null>");
        failed = 1;
        goto cleanup_thread;
    }
    free_result(&exports, &retval);
    text = NULL;

    args[0] = number_arg(8.0);
    args[1] = number_arg((double)session_handle);
    args[2] = number_arg(2000.0);
    args[3] = number_arg((double)(VIPC_APP_OPERATION_MIN + 72u));
    args[4] = number_arg(124.0);
    args[5] = number_arg(456.0);
    args[6] = string_arg(binary_probe_text);
    if (!call_string(&exports, args, 7, &text, &retval)
        || strcmp(text, "VIPC/1.0|ERR|DATA|TEXT_RESPONSE") != 0) {
        fprintf(
            stderr,
            "TRANSACT_TEXT binary response accepted: %s\n",
            text ? text : "<null>");
        failed = 1;
        goto cleanup_thread;
    }
    free_result(&exports, &retval);
    text = NULL;

    /*
     * Invalid UTF-8 must fail before a request is emitted. The overlong C0 AF
     * sequence is never valid UTF-8.
     */
    args[0] = number_arg(8.0);
    args[1] = number_arg((double)session_handle);
    args[2] = number_arg(2000.0);
    args[3] = number_arg((double)(VIPC_APP_OPERATION_MIN + 73u));
    args[4] = number_arg(125.0);
    args[5] = number_arg(456.0);
    args[6] = string_arg(invalid_utf8_request);
    if (!call_string(&exports, args, 7, &text, &retval)
        || strcmp(text, "VIPC/1.0|ERR|DATA|TEXT_REQUEST") != 0) {
        fprintf(
            stderr,
            "TRANSACT_TEXT invalid UTF-8 request accepted: %s\n",
            text ? text : "<null>");
        failed = 1;
        goto cleanup_thread;
    }
    free_result(&exports, &retval);
    text = NULL;

    args[0] = number_arg(7.0);
    args[1] = number_arg((double)session_handle);
    if (!call_string(&exports, args, 2, &text, &retval)
        || strcmp(text, "VIPC/1.0|OK|CLOSED") != 0) {
        fprintf(stderr, "CLOSE failed: %s\n", text ? text : "<null>");
        failed = 1;
        goto cleanup_thread;
    }
    free_result(&exports, &retval);
    text = NULL;

    /*
     * Closing increments the slot generation. The old numeric token must never
     * alias a future session.
     */
    args[0] = number_arg(2.0);
    args[1] = number_arg((double)session_handle);
    if (!call_string(&exports, args, 2, &text, &retval)
        || strcmp(text, "VIPC/1.0|ERR|STATE|INVALID_HANDLE") != 0) {
        fprintf(stderr, "stale handle accepted: %s\n", text ? text : "<null>");
        failed = 1;
        goto cleanup_thread;
    }
    free_result(&exports, &retval);
    text = NULL;

cleanup_thread:
    if (retval.type == ESABI_TYPE_STRING && retval.payload.string_value) {
        free_result(&exports, &retval);
    }
    if (server_thread) {
        wait_result = WaitForSingleObject(server_thread, 3500u);
        if (wait_result != WAIT_OBJECT_0) {
            fprintf(stderr, "server thread did not exit\n");
            failed = 1;
        }
        (void)CloseHandle(server_thread);
    }
    if (server_context.failed) {
        fprintf(stderr, "server validation failed\n");
        failed = 1;
    }

cleanup_server:
    if (server) vipc_server_destroy(server);

cleanup_exports:
    exports.terminate();
    unload_exports(&exports);
    return failed;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: externalobject_smoke <adapter-dll>\n");
        return 2;
    }

    if (run_test(argv[1]) != 0) return 1;
    printf("VectorIPC ExternalObject native ABI smoke: PASS\n");
    return 0;
}

#else

int main(void) {
    printf("VectorIPC ExternalObject smoke: SKIP (Windows only)\n");
    return 0;
}

#endif
