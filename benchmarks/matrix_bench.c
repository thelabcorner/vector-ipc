#include "vectoripc/vipc.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define MATRIX_TIMEOUT_MS 10000u
#define MATRIX_SERVER_TIMEOUT_MS 120000u
#define MATRIX_MAX_PAYLOAD (4u * 1024u * 1024u)

typedef struct bench_case {
    uint32_t payload_size;
    uint32_t warmup;
    uint32_t samples;
} bench_case;

static const bench_case k_cases[] = {
    { 0u,                    2000u, 20000u },
    { 1u,                    1000u, 15000u },
    { 8u,                    1000u, 15000u },
    { 32u,                   1000u, 15000u },
    { 128u,                  1000u, 15000u },
    { 512u,                  1000u, 12000u },
    { 1024u,                 1000u, 10000u },
    { 4096u,                  500u,  5000u },
    { 16384u,                 300u,  2500u },
    { 65536u,                 150u,  1000u },
    { 262144u,                 75u,   400u },
    { 1048576u,                25u,   100u },
    { MATRIX_MAX_PAYLOAD,       10u,    25u }
};

#define MATRIX_CASE_COUNT ((uint32_t)(sizeof(k_cases) / sizeof(k_cases[0])))

static int parse_u32(const char *text, uint32_t *out_value) {
    char *end = NULL;
    unsigned long value;
    if (!text || !out_value || text[0] == '\0') return 0;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value > UINT32_MAX) return 0;
    *out_value = (uint32_t)value;
    return 1;
}

static double ticks_to_us(LONGLONG ticks, LONGLONG frequency) {
    return ((double)ticks * 1000000.0) / (double)frequency;
}

static int compare_double(const void *left, const void *right) {
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

static uint8_t payload_byte(uint32_t size, uint32_t index) {
    uint32_t x = size * 2246822519u;
    x ^= index * 3266489917u;
    x ^= x >> 13;
    x *= 2654435761u;
    return (uint8_t)(x & 0xffu);
}

static void fill_payload(uint8_t *buffer, uint32_t size) {
    uint32_t i;
    for (i = 0u; i < size; ++i) buffer[i] = payload_byte(size, i);
}

static int spawn_server(
    const char *module_path,
    const char *endpoint,
    uint32_t iterations,
    PROCESS_INFORMATION *out_process) {
    STARTUPINFOA startup;
    char command_line[2048];
    int written;

    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(out_process, sizeof(*out_process));
    startup.cb = (DWORD)sizeof(startup);

    written = sprintf_s(
        command_line,
        sizeof(command_line),
        "\"%s\" --server %s %lu",
        module_path,
        endpoint,
        (unsigned long)iterations);
    if (written <= 0 || (size_t)written >= sizeof(command_line)) return 0;

    return CreateProcessA(
        NULL,
        command_line,
        NULL,
        NULL,
        FALSE,
        CREATE_NO_WINDOW,
        NULL,
        NULL,
        &startup,
        out_process) != FALSE;
}

static int wait_child_ok(PROCESS_INFORMATION *process, DWORD timeout_ms) {
    DWORD wait_result = WaitForSingleObject(process->hProcess, timeout_ms);
    DWORD exit_code = 1u;
    if (wait_result != WAIT_OBJECT_0) {
        (void)TerminateProcess(process->hProcess, 2u);
        (void)WaitForSingleObject(process->hProcess, 1000u);
        return 0;
    }
    return GetExitCodeProcess(process->hProcess, &exit_code) && exit_code == 0u;
}

static void close_process_handles(PROCESS_INFORMATION *process) {
    if (process->hThread) (void)CloseHandle(process->hThread);
    if (process->hProcess) (void)CloseHandle(process->hProcess);
    ZeroMemory(process, sizeof(*process));
}

static int child_server_main(const char *endpoint, uint32_t iterations) {
    vipc_server *server = NULL;
    vipc_channel *channel = NULL;
    vipc_error error;
    vipc_status status;
    uint8_t *payload = NULL;
    uint32_t i;
    int result = 1;

    payload = (uint8_t *)malloc(MATRIX_MAX_PAYLOAD);
    if (!payload) return 2;

    status = vipc_server_create(endpoint, &server, &error);
    if (status != VIPC_OK) goto cleanup;

    status = vipc_server_accept(
        server,
        MATRIX_SERVER_TIMEOUT_MS,
        &channel,
        &error);
    if (status != VIPC_OK) goto cleanup;

    for (i = 0u; i < iterations; ++i) {
        vipc_message request;
        vipc_message response;
        uint32_t payload_size = 0u;

        status = vipc_channel_receive(
            channel,
            &request,
            payload,
            MATRIX_MAX_PAYLOAD,
            &payload_size,
            MATRIX_SERVER_TIMEOUT_MS,
            &error);
        if (status != VIPC_OK) goto cleanup;

        response.kind = VIPC_KIND_RESPONSE;
        response.flags = VIPC_FLAG_NONE;
        response.operation = request.operation;
        response.correlation_id = request.correlation_id;
        response.payload_size = payload_size;

        status = vipc_channel_send(
            channel,
            &response,
            payload_size ? payload : NULL,
            MATRIX_SERVER_TIMEOUT_MS,
            &error);
        if (status != VIPC_OK) goto cleanup;
    }

    result = 0;

cleanup:
    if (channel) vipc_channel_destroy(channel);
    if (server) vipc_server_destroy(server);
    free(payload);
    return result;
}

static int round_trip(
    vipc_channel *channel,
    uint64_t correlation,
    const uint8_t *payload,
    uint32_t payload_size,
    uint8_t *receive_buffer) {
    vipc_message request;
    vipc_message response;
    vipc_error error;
    vipc_status status;
    uint32_t received = 0u;

    request.kind = VIPC_KIND_REQUEST;
    request.flags = VIPC_FLAG_NONE;
    request.operation = VIPC_APP_OPERATION_MIN + 90u;
    request.correlation_id = correlation;
    request.payload_size = payload_size;

    status = vipc_channel_send(
        channel,
        &request,
        payload_size ? payload : NULL,
        MATRIX_TIMEOUT_MS,
        &error);
    if (status != VIPC_OK) return 0;

    status = vipc_channel_receive(
        channel,
        &response,
        receive_buffer,
        MATRIX_MAX_PAYLOAD,
        &received,
        MATRIX_TIMEOUT_MS,
        &error);
    if (status != VIPC_OK
        || response.kind != VIPC_KIND_RESPONSE
        || response.operation != request.operation
        || response.correlation_id != correlation
        || received != payload_size) {
        return 0;
    }

    if (payload_size != 0u
        && memcmp(payload, receive_buffer, payload_size) != 0) {
        return 0;
    }
    return 1;
}

static uint32_t total_iterations(void) {
    uint32_t total = 0u;
    uint32_t i;
    for (i = 0u; i < MATRIX_CASE_COUNT; ++i) {
        total += k_cases[i].warmup + k_cases[i].samples;
    }
    return total;
}

static void print_case(
    uint32_t payload_size,
    double *samples,
    uint32_t count) {
    double sum = 0.0;
    double mean;
    double median;
    double p95;
    double p99;
    double maximum;
    double rps;
    double mib_per_second;
    uint32_t i;

    qsort(samples, count, sizeof(double), compare_double);
    for (i = 0u; i < count; ++i) sum += samples[i];
    mean = sum / (double)count;
    median = samples[count / 2u];
    p95 = samples[(uint32_t)((double)(count - 1u) * 0.95)];
    p99 = samples[(uint32_t)((double)(count - 1u) * 0.99)];
    maximum = samples[count - 1u];
    rps = 1000000.0 / mean;
    mib_per_second = payload_size == 0u
        ? 0.0
        : ((double)payload_size * 2.0 * rps) / (1024.0 * 1024.0);

    printf(
        "payload=%lu n=%lu mean_us=%.3f median_us=%.3f "
        "p95_us=%.3f p99_us=%.3f max_us=%.3f rps=%.0f duplex_mib_s=%.3f\n",
        (unsigned long)payload_size,
        (unsigned long)count,
        mean,
        median,
        p95,
        p99,
        maximum,
        rps,
        mib_per_second);
}

static int benchmark_main(void) {
    char module_path[MAX_PATH];
    char endpoint[80];
    DWORD module_length;
    LARGE_INTEGER frequency;
    PROCESS_INFORMATION process;
    vipc_channel *client = NULL;
    vipc_error error;
    vipc_status status;
    uint8_t *payload = NULL;
    uint8_t *receive_buffer = NULL;
    double *samples = NULL;
    uint64_t correlation = 1u;
    uint32_t case_index;
    int ok = 0;

    module_length = GetModuleFileNameA(NULL, module_path, (DWORD)sizeof(module_path));
    if (module_length == 0u || module_length >= (DWORD)sizeof(module_path)) return 2;
    if (!QueryPerformanceFrequency(&frequency)) return 2;

    payload = (uint8_t *)malloc(MATRIX_MAX_PAYLOAD);
    receive_buffer = (uint8_t *)malloc(MATRIX_MAX_PAYLOAD);
    samples = (double *)malloc(sizeof(double) * 20000u);
    if (!payload || !receive_buffer || !samples) goto cleanup;

    (void)sprintf_s(
        endpoint,
        sizeof(endpoint),
        "matrix-%lu",
        (unsigned long)GetCurrentProcessId());

    if (!spawn_server(module_path, endpoint, total_iterations(), &process)) {
        fprintf(stderr, "CreateProcess failed: %lu\n", (unsigned long)GetLastError());
        goto cleanup;
    }

    status = vipc_client_connect(endpoint, MATRIX_TIMEOUT_MS, &client, &error);
    if (status != VIPC_OK) {
        fprintf(stderr, "connect failed: %s\n", vipc_status_name(status));
        (void)TerminateProcess(process.hProcess, 3u);
        (void)WaitForSingleObject(process.hProcess, 1000u);
        close_process_handles(&process);
        goto cleanup;
    }

    printf("VectorIPC payload matrix\n");
    printf("topology=separate-helper persistent-channel max_payload=%u\n",
        (unsigned)MATRIX_MAX_PAYLOAD);

    for (case_index = 0u; case_index < MATRIX_CASE_COUNT; ++case_index) {
        const bench_case *bench = &k_cases[case_index];
        uint32_t i;

        fill_payload(payload, bench->payload_size);

        for (i = 0u; i < bench->warmup; ++i) {
            if (!round_trip(
                    client,
                    correlation++,
                    payload,
                    bench->payload_size,
                    receive_buffer)) {
                fprintf(stderr, "warmup failed at payload %lu\n",
                    (unsigned long)bench->payload_size);
                goto process_cleanup;
            }
        }

        for (i = 0u; i < bench->samples; ++i) {
            LARGE_INTEGER before;
            LARGE_INTEGER after;
            QueryPerformanceCounter(&before);
            if (!round_trip(
                    client,
                    correlation++,
                    payload,
                    bench->payload_size,
                    receive_buffer)) {
                fprintf(stderr, "sample failed at payload %lu index %lu\n",
                    (unsigned long)bench->payload_size,
                    (unsigned long)i);
                goto process_cleanup;
            }
            QueryPerformanceCounter(&after);
            samples[i] = ticks_to_us(
                after.QuadPart - before.QuadPart,
                frequency.QuadPart);
        }

        print_case(bench->payload_size, samples, bench->samples);
    }

    ok = 1;

process_cleanup:
    if (client) {
        vipc_channel_destroy(client);
        client = NULL;
    }
    if (!ok) {
        (void)TerminateProcess(process.hProcess, 4u);
        (void)WaitForSingleObject(process.hProcess, 1000u);
    } else if (!wait_child_ok(&process, MATRIX_TIMEOUT_MS)) {
        fprintf(stderr, "helper exit failed\n");
        ok = 0;
    }
    close_process_handles(&process);

cleanup:
    free(samples);
    free(receive_buffer);
    free(payload);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "--server") == 0) {
        uint32_t iterations = 0u;
        if (!vipc_endpoint_is_valid(argv[2])
            || !parse_u32(argv[3], &iterations)) {
            return 2;
        }
        return child_server_main(argv[2], iterations);
    }

    if (argc != 1) {
        fprintf(stderr, "usage: vectoripc_matrix_bench.exe\n");
        return 2;
    }
    return benchmark_main();
}

#else

int main(void) {
    fprintf(stderr, "VectorIPC matrix benchmark requires Windows.\n");
    return 2;
}

#endif
