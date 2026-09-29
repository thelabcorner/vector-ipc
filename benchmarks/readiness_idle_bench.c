#include "vectoripc/vipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define IDLE_DEFAULT_INTERVAL_MS 15000u
#define IDLE_DEFAULT_SAMPLES 2u
#define WAKE_DEFAULT_SPREAD_MS 200u
#define WAKE_DEFAULT_SAMPLES 40u
#define IDLE_DEFAULT_CPU_BUDGET_PERCENT 2.0
#define CALIBRATION_SPIN_MS 1000.0
#define CONTROL_SLEEP_CHUNK_MS 100u
#define CALIBRATION_MIN_RATIO 0.5
#define CALIBRATION_MAX_RATIO 2.0
#define CONNECT_TIMEOUT_MS 10000u
#define ACCEPT_TIMEOUT_MS 10000u
#define OP_TIMEOUT_MS 5000u
#define WAIT_TIMEOUT_MS 2000u
#define PING_PAYLOAD_BYTES 16u
#define MAX_SAMPLES 1000u
#define MAX_INTERVAL_MS 3600000u
#define TICKS_PER_MS 10000.0

typedef struct bench_peer {
    /* 8-byte aligned first member: written atomically by the peer thread. */
    volatile LONG64 send_qpc;
    vipc_server *server;
    HANDLE thread;
    HANDLE ready_event;
    HANDLE request_event;
    HANDLE cancel_event;
    HANDLE sent_event;
    volatile LONG delay_ms;
    LARGE_INTEGER frequency;
    uint64_t correlation;
    int failed;
} bench_peer;

typedef struct cpu_sample {
    double wall_ms;
    uint64_t thread_cpu_ticks;
    uint64_t process_cpu_ticks;
} cpu_sample;

typedef struct distribution {
    double min;
    double median;
    double p95;
    double max;
    double mean;
} distribution;

static LONG64 qpc_now(void) {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

static uint64_t filetime_ticks(const FILETIME *value) {
    return ((uint64_t)value->dwHighDateTime << 32) | (uint64_t)value->dwLowDateTime;
}

static double ticks_to_ms(uint64_t ticks) {
    return (double)ticks / TICKS_PER_MS;
}

static uint64_t gcd_u64(uint64_t a, uint64_t b) {
    while (b != 0u) {
        uint64_t next = a % b;
        a = b;
        b = next;
    }
    return a;
}

static int read_cpu_times(
    int thread_scope,
    FILETIME *out_kernel,
    FILETIME *out_user) {
    FILETIME creation;
    FILETIME exit;

    ZeroMemory(&creation, sizeof(creation));
    ZeroMemory(&exit, sizeof(exit));
    if (thread_scope) {
        return GetThreadTimes(
            GetCurrentThread(), &creation, &exit, out_kernel, out_user) ? 1 : 0;
    }
    return GetProcessTimes(
        GetCurrentProcess(), &creation, &exit, out_kernel, out_user) ? 1 : 0;
}

static uint64_t cpu_delta_ticks(
    const FILETIME *kernel_before,
    const FILETIME *user_before,
    const FILETIME *kernel_after,
    const FILETIME *user_after) {
    return (filetime_ticks(kernel_after) - filetime_ticks(kernel_before))
        + (filetime_ticks(user_after) - filetime_ticks(user_before));
}

static int compare_double(const void *left, const void *right) {
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

static void summarize(double *samples, uint32_t count, distribution *out) {
    double sum = 0.0;
    uint32_t i;

    qsort(samples, count, sizeof(double), compare_double);
    for (i = 0u; i < count; ++i) sum += samples[i];

    out->min = samples[0];
    out->median = samples[count / 2u];
    out->p95 = samples[(uint32_t)((double)(count - 1u) * 0.95)];
    out->max = samples[count - 1u];
    out->mean = sum / (double)count;
}

static void print_distribution(
    const char *label,
    const char *unit,
    double *samples,
    uint32_t count) {
    distribution stats;

    summarize(samples, count, &stats);
    printf(
        "  %s: n=%lu min=%.3f %s median=%.3f %s p95=%.3f %s max=%.3f %s "
        "mean=%.3f %s\n",
        label,
        (unsigned long)count,
        stats.min,
        unit,
        stats.median,
        unit,
        stats.p95,
        unit,
        stats.max,
        unit,
        stats.mean,
        unit);
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

static int parse_double(const char *text, double *out_value) {
    char *end = NULL;
    double value;

    if (!text || !out_value || text[0] == '\0') return 0;

    errno = 0;
    value = strtod(text, &end);
    if (errno != 0 || !end || *end != '\0' || value < 0.0) return 0;

    *out_value = value;
    return 1;
}

static void fill_probe_payload(uint8_t *buffer, uint32_t size, uint64_t sequence) {
    uint32_t i;
    for (i = 0u; i < size; ++i) {
        buffer[i] = (uint8_t)((sequence * 31u + i * 17u + 5u) & 0xffu);
    }
}

/*
 * Sleep() quantises to the system timer tick, which phase-locks the peer's send
 * to the same tick grid the readiness poll runs on and understates wake
 * latency. Spin on the shared QPC domain so the send lands at an arbitrary
 * point inside the waiter's window.
 */
static void spin_for_ms(const bench_peer *peer, uint32_t delay_ms) {
    LARGE_INTEGER target;
    LARGE_INTEGER now;

    if (delay_ms == 0u) return;
    target.QuadPart = qpc_now()
        + (LONGLONG)(((double)delay_ms * (double)peer->frequency.QuadPart)
            / 1000.0);
    for (;;) {
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= target.QuadPart) return;
    }
}

static DWORD WINAPI bench_peer_main(LPVOID opaque) {
    bench_peer *peer = (bench_peer *)opaque;
    vipc_channel *channel = NULL;
    vipc_error error;
    vipc_status status;
    uint8_t payload[PING_PAYLOAD_BYTES];
    HANDLE waits[2];
    DWORD result = 0u;

    status = vipc_server_accept(
        peer->server,
        ACCEPT_TIMEOUT_MS,
        &channel,
        &error);
    if (status != VIPC_OK) {
        peer->failed = 1;
        (void)SetEvent(peer->ready_event);
        return 1u;
    }
    (void)SetEvent(peer->ready_event);

    waits[0] = peer->request_event;
    waits[1] = peer->cancel_event;

    for (;;) {
        vipc_message message;
        DWORD delay_ms;
        DWORD wait_result = WaitForMultipleObjects(2u, waits, FALSE, INFINITE);

        if (wait_result != WAIT_OBJECT_0) break;

        (void)ResetEvent(peer->sent_event);
        delay_ms = (DWORD)InterlockedCompareExchange(&peer->delay_ms, 0, 0);
        spin_for_ms(peer, delay_ms);

        message.kind = VIPC_KIND_EVENT;
        message.flags = VIPC_FLAG_NONE;
        message.operation = VIPC_APP_OPERATION_MIN + 200u;
        message.correlation_id = peer->correlation;
        message.payload_size = (uint32_t)sizeof(payload);
        fill_probe_payload(payload, (uint32_t)sizeof(payload), peer->correlation);

        (void)InterlockedExchange64(&peer->send_qpc, qpc_now());
        status = vipc_channel_send(
            channel,
            &message,
            payload,
            OP_TIMEOUT_MS,
            &error);
        if (status != VIPC_OK) {
            peer->failed = 1;
            result = 1u;
        }
        ++peer->correlation;
        (void)SetEvent(peer->sent_event);

        if (peer->failed) break;
    }

    vipc_channel_destroy(channel);
    return result;
}

static int peer_start(bench_peer *peer, const char *endpoint) {
    DWORD thread_id = 0u;
    vipc_error error;
    vipc_status status;

    ZeroMemory(peer, sizeof(*peer));
    peer->correlation = 1u;
    if (!QueryPerformanceFrequency(&peer->frequency)) {
        fprintf(stderr, "idle bench QueryPerformanceFrequency failed\n");
        return 0;
    }

    status = vipc_server_create(endpoint, &peer->server, &error);
    if (status != VIPC_OK) {
        fprintf(
            stderr,
            "idle bench server_create: %s phase=%s winerr=%lu\n",
            vipc_status_name(status),
            vipc_phase_name(error.phase),
            (unsigned long)error.platform_code);
        return 0;
    }

    peer->ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    peer->request_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    peer->cancel_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    peer->sent_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!peer->ready_event || !peer->request_event
        || !peer->cancel_event || !peer->sent_event) {
        fprintf(stderr, "idle bench CreateEvent failed: %lu\n",
            (unsigned long)GetLastError());
        return 0;
    }

    peer->thread = CreateThread(NULL, 0, bench_peer_main, peer, 0, &thread_id);
    if (!peer->thread) {
        fprintf(stderr, "idle bench CreateThread failed: %lu\n",
            (unsigned long)GetLastError());
        return 0;
    }

    return 1;
}

static void peer_stop(bench_peer *peer) {
    DWORD wait_result = 1u;

    if (peer->cancel_event) (void)SetEvent(peer->cancel_event);
    if (peer->thread) {
        wait_result = WaitForSingleObject(peer->thread, OP_TIMEOUT_MS);
        (void)CloseHandle(peer->thread);
        peer->thread = NULL;
    }
    if (wait_result != WAIT_OBJECT_0 && !peer->failed) {
        fprintf(stderr, "idle bench peer thread did not exit cleanly\n");
        peer->failed = 1;
    }
    if (peer->ready_event) (void)CloseHandle(peer->ready_event);
    if (peer->request_event) (void)CloseHandle(peer->request_event);
    if (peer->cancel_event) (void)CloseHandle(peer->cancel_event);
    if (peer->sent_event) (void)CloseHandle(peer->sent_event);
    peer->ready_event = NULL;
    peer->request_event = NULL;
    peer->cancel_event = NULL;
    peer->sent_event = NULL;
    if (peer->server) {
        vipc_server_destroy(peer->server);
        peer->server = NULL;
    }
}

/*
 * Burn a known amount of wall time on this thread and report what the CPU
 * counter measured for it, so that an all-zero idle figure is distinguishable
 * from a counter that cannot resolve the real cost.
 */
static int calibrate_cpu_metric(LARGE_INTEGER frequency, double *out_ratio) {
    FILETIME kernel_before;
    FILETIME kernel_after;
    FILETIME user_before;
    FILETIME user_after;
    LARGE_INTEGER wall_before;
    LARGE_INTEGER wall_after;
    LARGE_INTEGER target;
    double wall_ms;
    double cpu_ms;

    if (!read_cpu_times(1, &kernel_before, &user_before)) return 0;
    (void)QueryPerformanceCounter(&wall_before);
    target.QuadPart = wall_before.QuadPart
        + (LONGLONG)(((double)CALIBRATION_SPIN_MS
            * (double)frequency.QuadPart) / 1000.0);
    for (;;) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= target.QuadPart) break;
    }
    (void)QueryPerformanceCounter(&wall_after);
    if (!read_cpu_times(1, &kernel_after, &user_after)) return 0;

    wall_ms = ((double)(wall_after.QuadPart - wall_before.QuadPart)
        * 1000.0) / (double)frequency.QuadPart;
    cpu_ms = ticks_to_ms(cpu_delta_ticks(
        &kernel_before, &user_before, &kernel_after, &user_after));
    *out_ratio = wall_ms > 0.0 ? cpu_ms / wall_ms : 0.0;
    return 1;
}

/*
 * Control window: the same process, the same peer, and the same wall duration as
 * the idle phase, but with no polling at all. A blocked thread must accrue no
 * CPU, so this separates the measured readiness cost from counter drift and
 * background-thread noise.
 */
static int measure_blocked_control(
    uint32_t total_ms,
    uint64_t *out_process_cpu_ticks,
    double *out_wall_ms,
    LARGE_INTEGER frequency) {
    FILETIME kernel_before;
    FILETIME kernel_after;
    FILETIME user_before;
    FILETIME user_after;
    LARGE_INTEGER wall_before;
    LARGE_INTEGER wall_after;
    ULONGLONG deadline;

    if (total_ms == 0u) return 0;
    if (!read_cpu_times(0, &kernel_before, &user_before)) return 0;
    (void)QueryPerformanceCounter(&wall_before);

    deadline = GetTickCount64() + (ULONGLONG)total_ms;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        DWORD remaining_ms;
        if (now >= deadline) break;
        remaining_ms = (DWORD)((deadline - now) > (ULONGLONG)CONTROL_SLEEP_CHUNK_MS
            ? (ULONGLONG)CONTROL_SLEEP_CHUNK_MS
            : (deadline - now));
        Sleep(remaining_ms);
    }

    (void)QueryPerformanceCounter(&wall_after);
    if (!read_cpu_times(0, &kernel_after, &user_after)) return 0;
    *out_process_cpu_ticks = cpu_delta_ticks(
        &kernel_before, &user_before, &kernel_after, &user_after);
    *out_wall_ms = ((double)(wall_after.QuadPart - wall_before.QuadPart)
        * 1000.0) / (double)frequency.QuadPart;
    return 1;
}

/*
 * Issue one readiness wait while the peer is silent, and capture the CPU the
 * waiting thread and the whole process actually burn for it.
 */
static int measure_idle_sample(
    vipc_channel *channel,
    uint32_t interval_ms,
    cpu_sample *out_sample) {
    FILETIME thread_kernel_before;
    FILETIME thread_kernel_after;
    FILETIME thread_user_before;
    FILETIME thread_user_after;
    FILETIME process_kernel_before;
    FILETIME process_kernel_after;
    FILETIME process_user_before;
    FILETIME process_user_after;
    LARGE_INTEGER wall_before;
    LARGE_INTEGER wall_after;
    LARGE_INTEGER frequency;
    vipc_error error;
    vipc_status status;

    if (!QueryPerformanceFrequency(&frequency)) return 0;
    if (!read_cpu_times(1, &thread_kernel_before, &thread_user_before)) return 0;
    if (!read_cpu_times(0, &process_kernel_before, &process_user_before)) return 0;

    (void)QueryPerformanceCounter(&wall_before);
    status = vipc_channel_wait_readable(channel, interval_ms, &error);
    (void)QueryPerformanceCounter(&wall_after);

    if (!read_cpu_times(1, &thread_kernel_after, &thread_user_after)) return 0;
    if (!read_cpu_times(0, &process_kernel_after, &process_user_after)) return 0;

    if (status != VIPC_ERR_TIMEOUT
        || error.phase != VIPC_PHASE_WAIT_READABLE
        || !vipc_channel_is_open(channel)) {
        fprintf(
            stderr,
            "idle wait: %s phase=%s winerr=%lu open=%d\n",
            vipc_status_name(status),
            vipc_phase_name(error.phase),
            (unsigned long)error.platform_code,
            vipc_channel_is_open(channel));
        return 0;
    }

    out_sample->wall_ms = ((double)(wall_after.QuadPart - wall_before.QuadPart)
        * 1000.0) / (double)frequency.QuadPart;
    out_sample->thread_cpu_ticks = cpu_delta_ticks(
        &thread_kernel_before,
        &thread_user_before,
        &thread_kernel_after,
        &thread_user_after);
    out_sample->process_cpu_ticks = cpu_delta_ticks(
        &process_kernel_before,
        &process_user_before,
        &process_kernel_after,
        &process_user_after);
    return 1;
}

/*
 * One wake-latency sample: the peer writes a frame `delay_ms` after the request,
 * so the wait observes the send at a deterministic offset inside its window.
 * Latency runs from the peer's pre-send timestamp (shared QPC domain) to the
 * waiter's return, and therefore includes the peer's own write.
 */
static int measure_wake_sample(
    bench_peer *peer,
    vipc_channel *channel,
    uint32_t delay_ms,
    double frequency,
    double *out_latency_ms) {
    vipc_error error;
    vipc_status status;
    vipc_message received;
    uint8_t payload[PING_PAYLOAD_BYTES];
    uint8_t expected[PING_PAYLOAD_BYTES];
    uint32_t payload_size = 0u;
    uint64_t correlation;
    LONG64 sent_qpc;
    LARGE_INTEGER wall_before;
    LARGE_INTEGER wall_after;
    DWORD wait_result;

    (void)ResetEvent(peer->sent_event);
    (void)InterlockedExchange(&peer->delay_ms, (LONG)delay_ms);
    correlation = peer->correlation;

    (void)SetEvent(peer->request_event);
    (void)QueryPerformanceCounter(&wall_before);
    status = vipc_channel_wait_readable(channel, WAIT_TIMEOUT_MS, &error);
    (void)QueryPerformanceCounter(&wall_after);

    if (status != VIPC_OK || error.phase != VIPC_PHASE_NONE) {
        fprintf(
            stderr,
            "wake wait: %s phase=%s winerr=%lu\n",
            vipc_status_name(status),
            vipc_phase_name(error.phase),
            (unsigned long)error.platform_code);
        return 0;
    }

    wait_result = WaitForSingleObject(peer->sent_event, OP_TIMEOUT_MS);
    if (wait_result != WAIT_OBJECT_0) {
        fprintf(stderr, "wake peer did not report a send\n");
        return 0;
    }

    sent_qpc = InterlockedCompareExchange64(&peer->send_qpc, 0, 0);
    if (sent_qpc <= 0 || sent_qpc > wall_after.QuadPart) {
        fprintf(
            stderr,
            "wake peer timestamp is not inside the wait window: "
            "sent_qpc=%lld wall_before=%lld wall_after=%lld status=%lu\n",
            (long long)sent_qpc,
            (long long)wall_before.QuadPart,
            (long long)wall_after.QuadPart,
            (unsigned long)status);
        return 0;
    }
    *out_latency_ms = ((double)(wall_after.QuadPart - sent_qpc)
        * 1000.0) / frequency;

    status = vipc_channel_receive(
        channel,
        &received,
        payload,
        (uint32_t)sizeof(payload),
        &payload_size,
        OP_TIMEOUT_MS,
        &error);
    if (status != VIPC_OK
        || received.kind != VIPC_KIND_EVENT
        || received.correlation_id != correlation
        || payload_size != (uint32_t)sizeof(payload)) {
        fprintf(
            stderr,
            "wake receive: %s phase=%s winerr=%lu\n",
            vipc_status_name(status),
            vipc_phase_name(error.phase),
            (unsigned long)error.platform_code);
        return 0;
    }

    fill_probe_payload(expected, (uint32_t)sizeof(expected), correlation);
    if (memcmp(payload, expected, sizeof(expected)) != 0) {
        fprintf(stderr, "wake payload mismatch\n");
        return 0;
    }

    return 1;
}

static int run_bench(
    uint32_t idle_interval_ms,
    uint32_t idle_samples,
    uint32_t wake_spread_ms,
    uint32_t wake_samples,
    double cpu_budget_percent) {
    bench_peer peer;
    vipc_channel *client = NULL;
    vipc_error error;
    vipc_status status;
    char endpoint[80];
    SYSTEM_INFO system_info;
    LARGE_INTEGER frequency;
    double *idle_wall = NULL;
    double *idle_thread = NULL;
    double *idle_process = NULL;
    double *idle_thread_percent = NULL;
    double *wake_latency = NULL;
    uint64_t *idle_thread_ticks = NULL;
    uint64_t thread_ticks_total = 0u;
    uint64_t quantum_ticks = 0u;
    double wall_total_ms = 0.0;
    double thread_share_percent = 0.0;
    double thread_share_upper_percent = 0.0;
    double wall_max_deviation_ms = 0.0;
    double calibration_ratio = 0.0;
    double control_wall_ms = 0.0;
    uint64_t control_ticks = 0u;
    uint32_t control_ms = 0u;
    distribution thread_percent_stats;
    distribution wall_stats;
    cpu_sample sample;
    uint32_t wake_total;
    uint32_t i;
    int failed = 0;

    ZeroMemory(&system_info, sizeof(system_info));
    GetSystemInfo(&system_info);
    if (!QueryPerformanceFrequency(&frequency)) {
        fprintf(stderr, "QueryPerformanceFrequency failed\n");
        return 1;
    }
    if (!calibrate_cpu_metric(frequency, &calibration_ratio)) {
        fprintf(stderr, "idle bench CPU metric calibration failed\n");
        return 1;
    }

    (void)sprintf_s(
        endpoint,
        sizeof(endpoint),
        "idle-bench-%lu",
        (unsigned long)GetCurrentProcessId());

    if (!peer_start(&peer, endpoint)) {
        peer_stop(&peer);
        return 1;
    }

    status = vipc_client_connect(endpoint, CONNECT_TIMEOUT_MS, &client, &error);
    if (status != VIPC_OK) {
        fprintf(
            stderr,
            "idle bench connect: %s phase=%s winerr=%lu\n",
            vipc_status_name(status),
            vipc_phase_name(error.phase),
            (unsigned long)error.platform_code);
        peer_stop(&peer);
        return 1;
    }

    if (WaitForSingleObject(peer.ready_event, CONNECT_TIMEOUT_MS)
        != WAIT_OBJECT_0) {
        fprintf(stderr, "idle bench peer never accepted\n");
        failed = 1;
        goto cleanup;
    }

    idle_wall = (double *)malloc(sizeof(double) * idle_samples);
    idle_thread = (double *)malloc(sizeof(double) * idle_samples);
    idle_process = (double *)malloc(sizeof(double) * idle_samples);
    idle_thread_percent = (double *)malloc(sizeof(double) * idle_samples);
    idle_thread_ticks = (uint64_t *)malloc(sizeof(uint64_t) * idle_samples);
    wake_latency = (double *)malloc(sizeof(double) * (wake_samples + 1u));
    if (!idle_wall || !idle_thread || !idle_process || !idle_thread_percent
        || !idle_thread_ticks || !wake_latency) {
        fprintf(stderr, "idle bench sample allocation failed\n");
        failed = 1;
        goto cleanup;
    }

    for (i = 0u; i < idle_samples; ++i) {
        if (!measure_idle_sample(client, idle_interval_ms, &sample)) {
            failed = 1;
            goto cleanup;
        }
        idle_thread_ticks[i] = sample.thread_cpu_ticks;
        thread_ticks_total += sample.thread_cpu_ticks;
        if (sample.thread_cpu_ticks != 0u) {
            quantum_ticks = gcd_u64(quantum_ticks, sample.thread_cpu_ticks);
        }
        idle_wall[i] = sample.wall_ms;
        idle_process[i] = ticks_to_ms(sample.process_cpu_ticks);
        idle_thread[i] = ticks_to_ms(sample.thread_cpu_ticks);
        wall_total_ms += sample.wall_ms;
        {
            double deviation = sample.wall_ms - (double)idle_interval_ms;
            if (deviation < 0.0) deviation = -deviation;
            if (deviation > wall_max_deviation_ms) {
                wall_max_deviation_ms = deviation;
            }
        }
        idle_thread_percent[i] = sample.wall_ms > 0.0
            ? (ticks_to_ms(sample.thread_cpu_ticks) * 100.0) / sample.wall_ms
            : 0.0;
    }

    /*
     * The idle phase ended on timeouts only. A full round trip now proves those
     * timeouts consumed no stream bytes and left the channel synchronized.
     */
    if (!measure_wake_sample(
            &peer,
            client,
            1u,
            (double)frequency.QuadPart,
            &wake_latency[0])) {
        failed = 1;
        goto cleanup;
    }
    wake_total = wake_samples + 1u;
    for (i = 1u; i < wake_total; ++i) {
        uint32_t delay_ms = 1u + (i * 7919u) % wake_spread_ms;
        if (!measure_wake_sample(
                &peer,
                client,
                delay_ms,
                (double)frequency.QuadPart,
                &wake_latency[i])) {
            failed = 1;
            goto cleanup;
        }
    }

    /* wall_total_ms is already milliseconds: match the control window to the
     * full idle phase so the two windows are directly comparable. */
    control_ms = (uint32_t)wall_total_ms;
    if (control_ms == 0u) control_ms = 1u;
    if (!measure_blocked_control(
            control_ms,
            &control_ticks,
            &control_wall_ms,
            frequency)) {
        fprintf(stderr, "idle bench blocked control failed\n");
        failed = 1;
        goto cleanup;
    }

    if (wall_total_ms > 0.0) {
        thread_share_percent =
            (ticks_to_ms(thread_ticks_total) * 100.0) / wall_total_ms;
        thread_share_upper_percent = (ticks_to_ms(
            thread_ticks_total + (uint64_t)idle_samples * quantum_ticks)
            * 100.0) / wall_total_ms;
    }

    summarize(idle_thread_percent, idle_samples, &thread_percent_stats);
    summarize(idle_wall, idle_samples, &wall_stats);

    printf("VectorIPC wait_readable idle benchmark\n");
    printf(
        "Environment: Windows logical processors=%lu QPC=%.0f Hz "
        "pointer_bits=%u\n",
        (unsigned long)system_info.dwNumberOfProcessors,
        (double)frequency.QuadPart,
        (unsigned)(sizeof(void *) * 8u));
    printf(
        "Topology: client channel and idle peer thread in one process; CPU is "
        "measured with GetThreadTimes/GetProcessTimes\n");
    printf(
        "CPU metric calibration: %.0f ms wall spin -> %.3f measured CPU/wall "
        "ratio (a live counter is expected near 1.0; Windows per-core thread "
        "accounting can over-report a spinning thread that migrates, which is "
        "why the idle figure below is reported as a bound and cross-checked "
        "against a blocked control)\n",
        CALIBRATION_SPIN_MS,
        calibration_ratio);
    printf(
        "Idle phase: peer sends nothing, n=%lu samples of %lu ms\n",
        (unsigned long)idle_samples,
        (unsigned long)idle_interval_ms);
    printf(
        "  correctness: %lu/%lu timeouts returned VIPC_ERR_TIMEOUT with "
        "phase=wait-readable and an open channel\n",
        (unsigned long)idle_samples,
        (unsigned long)idle_samples);
    print_distribution("wall per wait", "ms", idle_wall, idle_samples);
    printf(
        "  deadline accuracy: median wall/interval=%.4f, worst deviation from "
        "the requested %lu ms = %.3f ms\n",
        wall_stats.median / (double)idle_interval_ms,
        (unsigned long)idle_interval_ms,
        wall_max_deviation_ms);
    print_distribution("waiter thread CPU", "ms", idle_thread, idle_samples);
    print_distribution(
        "waiter thread CPU share of one core", "%",
        idle_thread_percent,
        idle_samples);
    print_distribution(
        "process CPU (all threads)", "ms", idle_process, idle_samples);
    printf(
        "  CPU counter quantum observed across samples: %.3f ms "
        "(thread CPU is quantised, so the true value lies in "
        "[reported, reported+quantum) per sample)\n",
        quantum_ticks == 0u ? 0.0 : ticks_to_ms(quantum_ticks));
    printf(
        "  aggregate over %.1f s of waiting: waiter thread CPU share of one "
        "core = %.4f%% (bounded upper bound %.4f%%)\n",
        wall_total_ms / 1000.0,
        thread_share_percent,
        thread_share_upper_percent);
    printf(
        "Wake phase: n=%lu samples, peer send offset spread over %lu ms\n",
        (unsigned long)wake_total,
        (unsigned long)wake_spread_ms);
    print_distribution("readiness wake latency", "ms", wake_latency, wake_total);
    printf(
        "  correctness: %lu/%lu waits returned VIPC_OK and the following "
        "receive returned the matching verified frame\n",
        (unsigned long)wake_total,
        (unsigned long)wake_total);
    printf(
        "Idle CPU budget: conservative upper bound must stay below %.3f%% of "
        "one core; observed bound=%.4f%%\n",
        cpu_budget_percent,
        thread_share_upper_percent);

    printf(
        "  blocked control: %.1f s of pure sleeping in the same process with "
        "the peer idle -> process CPU=%.3f ms (%.4f%% of one core)\n",
        control_wall_ms / 1000.0,
        ticks_to_ms(control_ticks),
        control_wall_ms > 0.0
            ? (ticks_to_ms(control_ticks) * 100.0) / control_wall_ms
            : 0.0);

    if (calibration_ratio < CALIBRATION_MIN_RATIO
        || calibration_ratio > CALIBRATION_MAX_RATIO) {
        fprintf(
            stderr,
            "CPU counter liveness check out of band: ratio=%.3f expected "
            "%.1f..%.1f\n",
            calibration_ratio,
            CALIBRATION_MIN_RATIO,
            CALIBRATION_MAX_RATIO);
        failed = 1;
    }

    if (thread_share_upper_percent > cpu_budget_percent) {
        fprintf(
            stderr,
            "idle CPU budget exceeded: bound=%.4f%% budget=%.3f%%\n",
            thread_share_upper_percent,
            cpu_budget_percent);
        failed = 1;
    }

    printf(
        "VectorIPC wait_readable idle benchmark: %s\n",
        failed ? "FAIL" : "PASS");

cleanup:
    if (client) vipc_channel_destroy(client);
    peer_stop(&peer);
    if (peer.failed) failed = 1;
    free(idle_thread_ticks);
    free(wake_latency);
    free(idle_thread_percent);
    free(idle_process);
    free(idle_thread);
    free(idle_wall);
    return failed ? 1 : 0;
}

int main(int argc, char **argv) {
    uint32_t idle_interval_ms = IDLE_DEFAULT_INTERVAL_MS;
    uint32_t idle_samples = IDLE_DEFAULT_SAMPLES;
    uint32_t wake_spread_ms = WAKE_DEFAULT_SPREAD_MS;
    uint32_t wake_samples = WAKE_DEFAULT_SAMPLES;
    double cpu_budget_percent = IDLE_DEFAULT_CPU_BUDGET_PERCENT;

    if (argc >= 2 && !parse_u32(argv[1], &idle_interval_ms)) return 2;
    if (argc >= 3 && !parse_u32(argv[2], &idle_samples)) return 2;
    if (argc >= 4 && !parse_u32(argv[3], &wake_spread_ms)) return 2;
    if (argc >= 5 && !parse_u32(argv[4], &wake_samples)) return 2;
    if (argc >= 6 && !parse_double(argv[5], &cpu_budget_percent)) return 2;
    if (argc > 5
        || idle_interval_ms == 0u || idle_interval_ms > MAX_INTERVAL_MS
        || idle_samples == 0u || idle_samples > MAX_SAMPLES
        || wake_spread_ms == 0u || wake_spread_ms > MAX_INTERVAL_MS
        || wake_samples == 0u || wake_samples + 1u > MAX_SAMPLES) {
        fprintf(
            stderr,
            "usage: vectoripc_readiness_idle_bench [idle-interval-ms] "
            "[idle-samples] [wake-spread-ms] [wake-samples] [cpu-budget-pct]\n");
        return 2;
    }

    return run_bench(
        idle_interval_ms,
        idle_samples,
        wake_spread_ms,
        wake_samples,
        cpu_budget_percent);
}

#else

int main(void) {
    fprintf(stderr, "VectorIPC readiness idle benchmark currently requires Windows.\n");
    return 2;
}

#endif
