#include "vectoripc/vipc.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define BENCH_COLD_SAMPLES 25u
#define BENCH_WARMUP 2000u
#define BENCH_ZERO_SAMPLES 20000u
#define BENCH_1K_SAMPLES 10000u
#define BENCH_MAX_PAYLOAD 1024u
#define BENCH_TIMEOUT_MS 5000u

static double ticks_to_us(LONGLONG ticks, LONGLONG frequency) {
    return ((double)ticks * 1000000.0) / (double)frequency;
}

static int compare_double(const void *left, const void *right) {
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

static void summarize(
    double *samples,
    uint32_t count,
    double *out_mean,
    double *out_median,
    double *out_p95) {
    double sum = 0.0;
    uint32_t i;

    qsort(samples, count, sizeof(double), compare_double);
    for (i = 0; i < count; ++i) sum += samples[i];

    *out_mean = sum / (double)count;
    *out_median = samples[count / 2u];
    *out_p95 = samples[(uint32_t)((double)(count - 1u) * 0.95)];
}

static void print_latency_distribution(
    const char *label,
    double *samples,
    uint32_t count) {
    double mean;
    double median;
    double p95;

    summarize(samples, count, &mean, &median, &p95);
    printf(
        "%s: n=%lu mean=%.3f us median=%.3f us p95=%.3f us\n",
        label,
        (unsigned long)count,
        mean,
        median,
        p95);
}

static void print_round_trip_distribution(
    const char *label,
    double *samples,
    uint32_t count,
    uint32_t payload_bytes) {
    double mean;
    double median;
    double p95;
    double round_trips_per_second;
    double payload_mib_per_second;

    summarize(samples, count, &mean, &median, &p95);
    round_trips_per_second = 1000000.0 / mean;
    payload_mib_per_second = payload_bytes == 0u
        ? 0.0
        : ((double)payload_bytes * 2.0 * round_trips_per_second)
            / (1024.0 * 1024.0);

    printf(
        "%s: n=%lu mean=%.3f us median=%.3f us p95=%.3f us %.0f round-trips/s",
        label,
        (unsigned long)count,
        mean,
        median,
        p95,
        round_trips_per_second);

    if (payload_bytes != 0u) {
        printf(" %.1f MiB/s user-payload duplex", payload_mib_per_second);
    }
    printf("\n");
}

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

static int child_server_main(const char *endpoint, uint32_t iterations) {
    vipc_server *server = NULL;
    vipc_channel *channel = NULL;
    vipc_error error;
    vipc_status status;
    vipc_message request;
    vipc_message response;
    uint8_t payload[BENCH_MAX_PAYLOAD];
    uint32_t payload_size = 0;
    uint32_t i;
    int result = 1;

    status = vipc_server_create(endpoint, &server, &error);
    if (status != VIPC_OK) {
        fprintf(
            stderr,
            "child server_create: %s phase=%s winerr=%lu\n",
            vipc_status_name(status),
            vipc_phase_name(error.phase),
            (unsigned long)error.platform_code);
        return 1;
    }

    status = vipc_server_accept(
        server,
        BENCH_TIMEOUT_MS,
        &channel,
        &error);
    if (status != VIPC_OK) {
        fprintf(
            stderr,
            "child accept: %s phase=%s winerr=%lu\n",
            vipc_status_name(status),
            vipc_phase_name(error.phase),
            (unsigned long)error.platform_code);
        goto cleanup;
    }

    for (i = 0; i < iterations; ++i) {
        status = vipc_channel_receive(
            channel,
            &request,
            payload,
            (uint32_t)sizeof(payload),
            &payload_size,
            BENCH_TIMEOUT_MS,
            &error);
        if (status != VIPC_OK) {
            fprintf(
                stderr,
                "child receive[%lu]: %s phase=%s winerr=%lu\n",
                (unsigned long)i,
                vipc_status_name(status),
                vipc_phase_name(error.phase),
                (unsigned long)error.platform_code);
            goto cleanup;
        }

        response.kind = VIPC_KIND_RESPONSE;
        response.flags = VIPC_FLAG_NONE;
        response.operation = request.operation;
        response.correlation_id = request.correlation_id;
        response.payload_size = payload_size;

        status = vipc_channel_send(
            channel,
            &response,
            payload_size ? payload : NULL,
            BENCH_TIMEOUT_MS,
            &error);
        if (status != VIPC_OK) {
            fprintf(
                stderr,
                "child send[%lu]: %s phase=%s winerr=%lu\n",
                (unsigned long)i,
                vipc_status_name(status),
                vipc_phase_name(error.phase),
                (unsigned long)error.platform_code);
            goto cleanup;
        }
    }

    result = 0;

cleanup:
    if (channel) vipc_channel_destroy(channel);
    vipc_server_destroy(server);
    return result;
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
    DWORD wait_result;
    DWORD exit_code = 1u;

    wait_result = WaitForSingleObject(process->hProcess, timeout_ms);
    if (wait_result != WAIT_OBJECT_0) {
        (void)TerminateProcess(process->hProcess, 2u);
        (void)WaitForSingleObject(process->hProcess, 1000u);
        return 0;
    }

    if (!GetExitCodeProcess(process->hProcess, &exit_code)) return 0;
    return exit_code == 0u;
}

static void close_process_handles(PROCESS_INFORMATION *process) {
    if (process->hThread) (void)CloseHandle(process->hThread);
    if (process->hProcess) (void)CloseHandle(process->hProcess);
    ZeroMemory(process, sizeof(*process));
}

static int round_trip(
    vipc_channel *channel,
    uint64_t correlation,
    const void *payload,
    uint32_t payload_size) {
    vipc_message request;
    vipc_message response;
    vipc_error error;
    uint8_t response_payload[BENCH_MAX_PAYLOAD];
    uint32_t response_size = 0;
    vipc_status status;

    request.kind = VIPC_KIND_REQUEST;
    request.flags = VIPC_FLAG_NONE;
    request.operation = VIPC_APP_OPERATION_MIN + 1u;
    request.correlation_id = correlation;
    request.payload_size = payload_size;

    status = vipc_channel_send(
        channel,
        &request,
        payload,
        BENCH_TIMEOUT_MS,
        &error);
    if (status != VIPC_OK) return 0;

    status = vipc_channel_receive(
        channel,
        &response,
        response_payload,
        (uint32_t)sizeof(response_payload),
        &response_size,
        BENCH_TIMEOUT_MS,
        &error);
    if (status != VIPC_OK) return 0;

    if (response.kind != VIPC_KIND_RESPONSE
        || response.operation != request.operation
        || response.correlation_id != correlation
        || response_size != payload_size) {
        return 0;
    }

    if (payload_size != 0u
        && memcmp(payload, response_payload, payload_size) != 0) {
        return 0;
    }

    return 1;
}

static int benchmark_cold_starts(
    const char *module_path,
    LONGLONG frequency,
    double samples[BENCH_COLD_SAMPLES]) {
    uint32_t i;

    for (i = 0; i < BENCH_COLD_SAMPLES; ++i) {
        char endpoint[80];
        PROCESS_INFORMATION process;
        vipc_channel *channel = NULL;
        vipc_error error;
        vipc_status status;
        LARGE_INTEGER before;
        LARGE_INTEGER after;

        (void)sprintf_s(
            endpoint,
            sizeof(endpoint),
            "bench-cold-%lu-%lu",
            (unsigned long)GetCurrentProcessId(),
            (unsigned long)i);

        QueryPerformanceCounter(&before);
        if (!spawn_server(module_path, endpoint, 0u, &process)) {
            fprintf(stderr, "CreateProcess cold[%lu] failed: %lu\n",
                (unsigned long)i, (unsigned long)GetLastError());
            return 0;
        }

        status = vipc_client_connect(
            endpoint,
            BENCH_TIMEOUT_MS,
            &channel,
            &error);
        QueryPerformanceCounter(&after);

        if (status != VIPC_OK) {
            fprintf(
                stderr,
                "cold connect[%lu]: %s phase=%s winerr=%lu\n",
                (unsigned long)i,
                vipc_status_name(status),
                vipc_phase_name(error.phase),
                (unsigned long)error.platform_code);
            (void)TerminateProcess(process.hProcess, 3u);
            (void)WaitForSingleObject(process.hProcess, 1000u);
            close_process_handles(&process);
            return 0;
        }

        samples[i] = ticks_to_us(
            after.QuadPart - before.QuadPart,
            frequency);

        vipc_channel_destroy(channel);
        if (!wait_child_ok(&process, BENCH_TIMEOUT_MS)) {
            fprintf(stderr, "cold child[%lu] failed\n", (unsigned long)i);
            close_process_handles(&process);
            return 0;
        }
        close_process_handles(&process);
    }

    return 1;
}

static int benchmark_persistent_helper(
    const char *module_path,
    LONGLONG frequency,
    double zero_samples[BENCH_ZERO_SAMPLES],
    double one_k_samples[BENCH_1K_SAMPLES]) {
    char endpoint[80];
    PROCESS_INFORMATION process;
    vipc_channel *client = NULL;
    vipc_error error;
    vipc_status status;
    uint8_t payload_1k[BENCH_MAX_PAYLOAD];
    uint64_t correlation = 1u;
    uint32_t total_iterations =
        BENCH_WARMUP + BENCH_ZERO_SAMPLES + BENCH_1K_SAMPLES;
    uint32_t i;
    int ok = 0;

    (void)sprintf_s(
        endpoint,
        sizeof(endpoint),
        "bench-warm-%lu",
        (unsigned long)GetCurrentProcessId());

    for (i = 0; i < BENCH_MAX_PAYLOAD; ++i) {
        payload_1k[i] = (uint8_t)(i * 37u + 11u);
    }

    if (!spawn_server(module_path, endpoint, total_iterations, &process)) {
        fprintf(stderr, "CreateProcess warm failed: %lu\n",
            (unsigned long)GetLastError());
        return 0;
    }

    status = vipc_client_connect(
        endpoint,
        BENCH_TIMEOUT_MS,
        &client,
        &error);
    if (status != VIPC_OK) {
        fprintf(
            stderr,
            "warm connect: %s phase=%s winerr=%lu\n",
            vipc_status_name(status),
            vipc_phase_name(error.phase),
            (unsigned long)error.platform_code);
        goto cleanup;
    }

    for (i = 0; i < BENCH_WARMUP; ++i) {
        if (!round_trip(client, correlation++, NULL, 0u)) goto cleanup;
    }

    for (i = 0; i < BENCH_ZERO_SAMPLES; ++i) {
        LARGE_INTEGER before;
        LARGE_INTEGER after;

        QueryPerformanceCounter(&before);
        if (!round_trip(client, correlation++, NULL, 0u)) goto cleanup;
        QueryPerformanceCounter(&after);

        zero_samples[i] = ticks_to_us(
            after.QuadPart - before.QuadPart,
            frequency);
    }

    for (i = 0; i < BENCH_1K_SAMPLES; ++i) {
        LARGE_INTEGER before;
        LARGE_INTEGER after;

        QueryPerformanceCounter(&before);
        if (!round_trip(
                client,
                correlation++,
                payload_1k,
                (uint32_t)sizeof(payload_1k))) {
            goto cleanup;
        }
        QueryPerformanceCounter(&after);

        one_k_samples[i] = ticks_to_us(
            after.QuadPart - before.QuadPart,
            frequency);
    }

    ok = 1;

cleanup:
    if (client) vipc_channel_destroy(client);

    if (!ok) {
        (void)TerminateProcess(process.hProcess, 4u);
        (void)WaitForSingleObject(process.hProcess, 1000u);
    } else if (!wait_child_ok(&process, BENCH_TIMEOUT_MS)) {
        fprintf(stderr, "warm child failed\n");
        ok = 0;
    }

    close_process_handles(&process);
    return ok;
}

static int benchmark_main(void) {
    char module_path[MAX_PATH];
    DWORD module_length;
    LARGE_INTEGER frequency;
    double cold_samples[BENCH_COLD_SAMPLES];
    double *zero_samples;
    double *one_k_samples;
    int ok = 0;

    module_length = GetModuleFileNameA(
        NULL,
        module_path,
        (DWORD)sizeof(module_path));
    if (module_length == 0u || module_length >= (DWORD)sizeof(module_path)) {
        fprintf(stderr, "GetModuleFileName failed: %lu\n",
            (unsigned long)GetLastError());
        return 2;
    }

    if (!QueryPerformanceFrequency(&frequency)) return 2;

    zero_samples = (double *)malloc(sizeof(double) * BENCH_ZERO_SAMPLES);
    one_k_samples = (double *)malloc(sizeof(double) * BENCH_1K_SAMPLES);
    if (!zero_samples || !one_k_samples) {
        free(zero_samples);
        free(one_k_samples);
        return 2;
    }

    if (!benchmark_cold_starts(
            module_path,
            frequency.QuadPart,
            cold_samples)) {
        goto cleanup;
    }

    if (!benchmark_persistent_helper(
            module_path,
            frequency.QuadPart,
            zero_samples,
            one_k_samples)) {
        goto cleanup;
    }

    printf("VectorIPC persistent named-pipe microbenchmark\n");
    printf("Topology: parent client <-> separate helper process, same Windows session\n");
    print_latency_distribution(
        "helper spawn -> connected",
        cold_samples,
        BENCH_COLD_SAMPLES);
    print_round_trip_distribution(
        "warm 0 B payload",
        zero_samples,
        BENCH_ZERO_SAMPLES,
        0u);
    print_round_trip_distribution(
        "warm 1024 B payload",
        one_k_samples,
        BENCH_1K_SAMPLES,
        1024u);

    ok = 1;

cleanup:
    free(zero_samples);
    free(one_k_samples);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "--server") == 0) {
        uint32_t iterations = 0;
        if (!vipc_endpoint_is_valid(argv[2]) || !parse_u32(argv[3], &iterations)) {
            return 2;
        }
        return child_server_main(argv[2], iterations);
    }

    if (argc != 1) {
        fprintf(stderr, "usage: vectoripc_ping_bench.exe\n");
        return 2;
    }

    return benchmark_main();
}

#else

int main(void) {
    fprintf(stderr, "VectorIPC ping benchmark currently requires Windows.\n");
    return 2;
}

#endif
