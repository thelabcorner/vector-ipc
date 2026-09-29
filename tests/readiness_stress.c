#include "vectoripc/vipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define DEFAULT_IDLE_ROUNDS 25u
#define DEFAULT_IDLE_MS 15u
#define DEFAULT_LONG_IDLE_ROUNDS 6u
#define DEFAULT_LONG_IDLE_MS 2000u
#define DEADLINE_TOLERANCE_MS 250u
#define BLOCKING_TIMEOUT_MS 3000u
#define BLOCKER_SETTLE_MS 50u
#define BUSY_PROBE_ATTEMPTS 500u
#define BUSY_REJECT_MAX_MS 50u
#define PEER_CLOSE_BOUND_MS 1000u
#define PEER_CLOSE_INFLIGHT_MS 150u
#define ABRUPT_EXIT_CODE 7u
#define ABRUPT_PEER_LIFETIME_MS 250u
#define CONNECT_TIMEOUT_MS 5000u
#define ACCEPT_TIMEOUT_MS 5000u
#define OP_TIMEOUT_MS 5000u
#define DETECT_TIMEOUT_MS 10000u
#define RECEIVE_PROBE_TIMEOUT_MS 5u
#define FRAME_PAYLOAD_BYTES 48u
#define MAX_IDLE_MS 600000u
#define MAX_ROUNDS 10000u

#define OP_REPEATED_IDLE 210u
#define OP_PEER_CLOSE 211u
#define OP_BUSY_PROBE 212u
#define OP_LONG_IDLE 213u
#define OP_REPLY 214u

static int g_failures = 0;

#define CHECK(condition, label) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", (label), __LINE__); \
        ++g_failures; \
    } \
} while (0)

static int parse_u32(const char *text, uint32_t *out_value) {
    char *end = NULL;
    unsigned long value;

    if (!text || !out_value || text[0] == '\0') return 0;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0
        || !end || *end != '\0'
        || value == 0u || value > UINT32_MAX) {
        return 0;
    }

    *out_value = (uint32_t)value;
    return 1;
}

static uint8_t pattern_byte(uint64_t sequence, uint32_t index) {
    uint32_t x = (uint32_t)(sequence * 2246822519u);
    x ^= index * 3266489917u;
    x ^= x >> 13;
    return (uint8_t)(x & 0xffu);
}

static void fill_pattern(uint8_t *buffer, uint32_t size, uint64_t sequence) {
    uint32_t i;
    for (i = 0u; i < size; ++i) buffer[i] = pattern_byte(sequence, i);
}

static void build_frame(
    vipc_message *message,
    uint32_t operation_offset,
    uint64_t correlation,
    uint32_t payload_size) {
    message->kind = VIPC_KIND_REQUEST;
    message->flags = VIPC_FLAG_NONE;
    message->operation = VIPC_APP_OPERATION_MIN + operation_offset;
    message->correlation_id = correlation;
    message->payload_size = payload_size;
}

static int verify_frame(
    const vipc_message *message,
    const uint8_t *payload,
    uint32_t payload_size,
    uint32_t operation_offset,
    uint64_t correlation) {
    uint8_t expected[FRAME_PAYLOAD_BYTES];

    if (message->kind != VIPC_KIND_REQUEST
        || message->operation != VIPC_APP_OPERATION_MIN + operation_offset
        || message->correlation_id != correlation
        || payload_size != (uint32_t)sizeof(expected)) {
        return 0;
    }
    fill_pattern(expected, (uint32_t)sizeof(expected), correlation);
    return memcmp(payload, expected, sizeof(expected)) == 0;
}

static void report_status(const char *label, vipc_status status,
                          const vipc_error *error) {
    fprintf(
        stderr,
        "%s: status=%s phase=%s platform_code=%lu\n",
        label,
        vipc_status_name(status),
        vipc_phase_name(error->phase),
        (unsigned long)error->platform_code);
}

static void make_endpoint(
    char *buffer,
    size_t capacity,
    const char *tag,
    uint32_t index) {
    (void)sprintf_s(
        buffer,
        capacity,
        "rdy%s-%lu-%lu",
        tag,
        (unsigned long)GetCurrentProcessId(),
        (unsigned long)index);
}

/*
 * Peer command set: send one framed batch when told, or close/quit. One peer
 * thread type drives every readiness scenario so the client side stays the only
 * place that makes timing claims.
 */
typedef struct peer_context {
    vipc_server *server;
    HANDLE thread;
    HANDLE ready_event;
    HANDLE send_event;
    HANDLE close_event;
    HANDLE quit_event;
    HANDLE sent_event;
    uint32_t operation_offset;
    uint32_t frames_per_command;
    uint32_t close_delay_ms;
    uint32_t sent_frames;
    int failed;
} peer_context;

static DWORD WINAPI peer_thread_main(LPVOID opaque) {
    peer_context *context = (peer_context *)opaque;
    vipc_channel *channel = NULL;
    vipc_error error;
    vipc_status status;
    HANDLE waits[3];

    status = vipc_server_accept(
        context->server,
        ACCEPT_TIMEOUT_MS,
        &channel,
        &error);
    if (status != VIPC_OK) {
        context->failed = 1;
        (void)SetEvent(context->ready_event);
        return 1u;
    }
    (void)SetEvent(context->ready_event);

    waits[0] = context->send_event;
    waits[1] = context->close_event;
    waits[2] = context->quit_event;

    for (;;) {
        DWORD wait_result = WaitForMultipleObjects(3u, waits, FALSE, INFINITE);
        vipc_message message;
        uint8_t payload[FRAME_PAYLOAD_BYTES];
        uint32_t frame;

        if (wait_result == WAIT_OBJECT_0 + 1u) {
            /* Delay the teardown so the client's readiness wait is provably
             * already in flight when the peer disappears. */
            if (context->close_delay_ms != 0u) {
                Sleep(context->close_delay_ms);
            }
            vipc_channel_destroy(channel);
            return context->failed ? 1u : 0u;
        }
        if (wait_result != WAIT_OBJECT_0) return 0u;

        (void)ResetEvent(context->sent_event);
        for (frame = 0u; frame < context->frames_per_command; ++frame) {
            uint64_t correlation = (uint64_t)context->sent_frames + 1u;
            build_frame(
                &message,
                context->operation_offset,
                correlation,
                (uint32_t)sizeof(payload));
            fill_pattern(payload, (uint32_t)sizeof(payload), correlation);
            status = vipc_channel_send(
                channel, &message, payload, OP_TIMEOUT_MS, &error);
            if (status != VIPC_OK) {
                report_status("peer send", status, &error);
                context->failed = 1;
                break;
            }
            ++context->sent_frames;
        }
        (void)SetEvent(context->sent_event);
        if (context->failed) return 1u;
    }
}

static int peer_start(
    peer_context *context,
    const char *endpoint,
    uint32_t operation_offset,
    uint32_t frames_per_command,
    uint32_t close_delay_ms) {
    vipc_error error;
    vipc_status status;
    DWORD thread_id = 0u;

    ZeroMemory(context, sizeof(*context));
    context->operation_offset = operation_offset;
    context->frames_per_command = frames_per_command;
    context->close_delay_ms = close_delay_ms;

    status = vipc_server_create(endpoint, &context->server, &error);
    if (status != VIPC_OK) {
        report_status("peer server_create", status, &error);
        return 0;
    }

    context->ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    /* Auto-reset: one SetEvent must mean exactly one send batch, otherwise the
     * peer re-runs a satisfied command in a tight loop. */
    context->send_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    context->close_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    context->quit_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    context->sent_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!context->ready_event || !context->send_event
        || !context->close_event || !context->quit_event
        || !context->sent_event) {
        fprintf(stderr, "peer CreateEvent failed: %lu\n",
            (unsigned long)GetLastError());
        return 0;
    }

    context->thread = CreateThread(
        NULL, 0, peer_thread_main, context, 0, &thread_id);
    if (!context->thread) {
        fprintf(stderr, "peer CreateThread failed: %lu\n",
            (unsigned long)GetLastError());
        return 0;
    }
    return 1;
}

static void peer_request_send(peer_context *context) {
    if (context->send_event) (void)SetEvent(context->send_event);
}

static void peer_request_close(peer_context *context) {
    if (context->close_event) (void)SetEvent(context->close_event);
}

static void peer_finish(peer_context *context) {
    DWORD wait_result = 1u;

    if (context->quit_event) (void)SetEvent(context->quit_event);
    if (context->thread) {
        wait_result = WaitForSingleObject(context->thread, OP_TIMEOUT_MS);
        if (wait_result != WAIT_OBJECT_0) {
            fprintf(stderr, "peer thread did not exit\n");
            ++g_failures;
        }
        (void)CloseHandle(context->thread);
        context->thread = NULL;
    }
    if (context->failed) {
        fprintf(stderr, "peer reported a failure\n");
        ++g_failures;
    }
    if (context->ready_event) (void)CloseHandle(context->ready_event);
    if (context->send_event) (void)CloseHandle(context->send_event);
    if (context->close_event) (void)CloseHandle(context->close_event);
    if (context->quit_event) (void)CloseHandle(context->quit_event);
    if (context->sent_event) (void)CloseHandle(context->sent_event);
    context->ready_event = NULL;
    context->send_event = NULL;
    context->close_event = NULL;
    context->quit_event = NULL;
    context->sent_event = NULL;
    if (context->server) {
        vipc_server_destroy(context->server);
        context->server = NULL;
    }
}

static int peer_connect(
    peer_context *context,
    const char *endpoint,
    vipc_channel **out_client) {
    vipc_error error;
    vipc_status status;

    status = vipc_client_connect(
        endpoint, CONNECT_TIMEOUT_MS, out_client, &error);
    if (status != VIPC_OK) {
        report_status("client connect", status, &error);
        return 0;
    }
    if (WaitForSingleObject(context->ready_event, CONNECT_TIMEOUT_MS)
        != WAIT_OBJECT_0) {
        fprintf(stderr, "peer never accepted\n");
        return 0;
    }
    return 1;
}

/*
 * Evidence 1: many consecutive non-destructive readiness timeouts, then a
 * successful duplex send/receive over the very same channel handle.
 */
static void test_repeated_idle_timeouts(uint32_t rounds, uint32_t idle_ms) {
    char endpoint[96];
    peer_context peer;
    vipc_channel *client = NULL;
    vipc_error error;
    vipc_status status;
    vipc_message message;
    uint8_t payload[FRAME_PAYLOAD_BYTES];
    uint8_t reply[FRAME_PAYLOAD_BYTES];
    uint32_t payload_size = 0u;
    uint32_t peer_pid;
    uint32_t peer_session;
    uint32_t i;
    ULONGLONG total_idle = 0u;
    int timeouts_ok = 1;

    make_endpoint(endpoint, sizeof(endpoint), "idle", 1u);
    if (!peer_start(&peer, endpoint, OP_REPEATED_IDLE, 1u, 0u)) {
        peer_finish(&peer);
        return;
    }
    if (!peer_connect(&peer, endpoint, &client)) {
        peer_finish(&peer);
        return;
    }
    peer_pid = vipc_channel_peer_pid(client);
    peer_session = vipc_channel_peer_session_id(client);

    for (i = 0u; i < rounds; ++i) {
        ULONGLONG started = GetTickCount64();
        ULONGLONG elapsed;

        status = vipc_channel_wait_readable(client, idle_ms, &error);
        elapsed = GetTickCount64() - started;
        total_idle += elapsed;

        if (status != VIPC_ERR_TIMEOUT
            || error.phase != VIPC_PHASE_WAIT_READABLE
            || error.platform_code != (uint32_t)ERROR_SEM_TIMEOUT
            || vipc_channel_is_open(client) != 1
            || vipc_channel_peer_pid(client) != peer_pid
            || vipc_channel_peer_session_id(client) != peer_session) {
            report_status("repeated idle timeout", status, &error);
            timeouts_ok = 0;
            break;
        }
        /* The deadline may land a tick early, but a readiness wait must never
         * return long before the interval it was asked to cover. */
        if (elapsed + DEADLINE_TOLERANCE_MS < (ULONGLONG)idle_ms) {
            fprintf(stderr, "idle timeout returned after only %lu ms\n",
                (unsigned long)elapsed);
            timeouts_ok = 0;
            break;
        }
    }
    CHECK(timeouts_ok, "repeated idle timeouts stay non-destructive");
    CHECK(vipc_channel_is_open(client) == 1,
        "repeated idle timeouts preserve the channel");
    printf(
        "  repeated idle timeouts: %lu rounds of %lu ms, %.2f s total\n",
        (unsigned long)rounds,
        (unsigned long)idle_ms,
        (double)total_idle / 1000.0);

    peer_request_send(&peer);
    status = vipc_channel_wait_readable(client, DETECT_TIMEOUT_MS, &error);
    if (status != VIPC_OK) {
        report_status("post-idle wait_readable", status, &error);
    }
    CHECK(status == VIPC_OK, "readiness observed after repeated idle timeouts");

    status = vipc_channel_receive(
        client,
        &message,
        payload,
        (uint32_t)sizeof(payload),
        &payload_size,
        OP_TIMEOUT_MS,
        &error);
    if (status != VIPC_OK) report_status("post-idle receive", status, &error);
    CHECK(status == VIPC_OK, "receive succeeds after repeated idle timeouts");
    CHECK(verify_frame(&message, payload, payload_size, OP_REPEATED_IDLE, 1u),
        "frame received after repeated idle timeouts is intact and in order");

    build_frame(&message, OP_REPLY, UINT64_C(0xF00D),
                (uint32_t)sizeof(reply));
    fill_pattern(reply, (uint32_t)sizeof(reply), UINT64_C(0xF00D));
    status = vipc_channel_send(client, &message, reply, OP_TIMEOUT_MS, &error);
    if (status != VIPC_OK) report_status("post-idle send", status, &error);
    CHECK(status == VIPC_OK, "send succeeds after repeated idle timeouts");
    CHECK(vipc_channel_is_open(client) == 1,
        "channel open after a post-idle duplex exchange");

    vipc_channel_destroy(client);
    peer_finish(&peer);
}

static int spawn_abrupt_peer(const char *endpoint, uint32_t lifetime_ms,
                             PROCESS_INFORMATION *out_process) {
    char module_path[MAX_PATH];
    char command_line[MAX_PATH + 128u];
    STARTUPINFOA startup;

    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(out_process, sizeof(*out_process));
    startup.cb = (DWORD)sizeof(startup);

    if (GetModuleFileNameA(NULL, module_path, (DWORD)sizeof(module_path)) == 0u
        || module_path[sizeof(module_path) - 1u] != '\0') {
        return 0;
    }
    if (sprintf_s(
            command_line,
            sizeof(command_line),
            "\"%s\" --abrupt-peer %s %lu",
            module_path,
            endpoint,
            (unsigned long)lifetime_ms) < 0) {
        return 0;
    }
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

/*
 * Evidence 2: a peer that goes away must be reported by wait_readable with the
 * exact status/phase instead of blocking until the deadline.
 */
static void test_peer_close_detection(void) {
    char endpoint[96];
    peer_context peer;
    vipc_channel *client = NULL;
    vipc_error error;
    vipc_status status;
    vipc_message message;
    uint8_t payload[FRAME_PAYLOAD_BYTES];
    uint32_t payload_size = 0u;
    ULONGLONG started;
    ULONGLONG elapsed;

    make_endpoint(endpoint, sizeof(endpoint), "close", 2u);
    if (!peer_start(&peer, endpoint, OP_PEER_CLOSE, 1u, PEER_CLOSE_INFLIGHT_MS)) {
        peer_finish(&peer);
        return;
    }
    if (!peer_connect(&peer, endpoint, &client)) {
        peer_finish(&peer);
        return;
    }

    /* Clean channel teardown by the peer. */
    peer_request_close(&peer);
    started = GetTickCount64();
    status = vipc_channel_wait_readable(client, DETECT_TIMEOUT_MS, &error);
    elapsed = GetTickCount64() - started;
    if (status != VIPC_ERR_PEER_CLOSED) {
        report_status("peer close wait_readable", status, &error);
    }
    CHECK(status == VIPC_ERR_PEER_CLOSED,
        "wait_readable reports a peer that closed its channel");
    CHECK(error.phase == VIPC_PHASE_WAIT_READABLE,
        "peer close phase is wait-readable");
    CHECK(error.platform_code == (uint32_t)ERROR_BROKEN_PIPE
            || error.platform_code == (uint32_t)ERROR_PIPE_NOT_CONNECTED
            || error.platform_code == (uint32_t)ERROR_NO_DATA,
        "peer close platform code is a pipe teardown code");
    CHECK(elapsed >= (ULONGLONG)PEER_CLOSE_INFLIGHT_MS,
        "the peer closes while the readiness wait is already in flight");
    CHECK(elapsed < (ULONGLONG)PEER_CLOSE_INFLIGHT_MS
            + (ULONGLONG)PEER_CLOSE_BOUND_MS,
        "peer close is detected without waiting for the deadline");
    CHECK(vipc_channel_is_open(client) == 0,
        "a definitive peer-close failure closes the channel");
    printf(
        "  peer close detected in %lu ms (platform_code=%lu, deadline %lu ms)\n",
        (unsigned long)elapsed,
        (unsigned long)error.platform_code,
        (unsigned long)DETECT_TIMEOUT_MS);

    /* Liveness must stay honest: nothing may look usable after the close. */
    status = vipc_channel_wait_readable(client, 10u, &error);
    CHECK(status == VIPC_ERR_NOT_CONNECTED,
        "a readiness wait after peer close reports not-connected");
    CHECK(error.phase == VIPC_PHASE_WAIT_READABLE,
        "not-connected readiness phase is still wait-readable");
    status = vipc_channel_send(client, &message, payload, 10u, &error);
    CHECK(status == VIPC_ERR_NOT_CONNECTED,
        "send after peer close reports not-connected");
    status = vipc_channel_receive(
        client, &message, payload, (uint32_t)sizeof(payload), &payload_size,
        10u, &error);
    CHECK(status == VIPC_ERR_NOT_CONNECTED,
        "receive after peer close reports not-connected");

    vipc_channel_destroy(client);
    peer_finish(&peer);

    /* Abrupt peer death: the helper process exits without any teardown. */
    {
        PROCESS_INFORMATION process;
        DWORD wait_result;
        DWORD exit_code = 0u;

        make_endpoint(endpoint, sizeof(endpoint), "exit", 3u);
        CHECK(spawn_abrupt_peer(endpoint, ABRUPT_PEER_LIFETIME_MS, &process),
            "abrupt peer process started");
        if (process.hProcess == NULL) return;

        status = vipc_client_connect(
            endpoint, CONNECT_TIMEOUT_MS, &client, &error);
        if (status != VIPC_OK) {
            report_status("abrupt peer connect", status, &error);
        }
        CHECK(status == VIPC_OK, "connect to a peer that never accepts");

        started = GetTickCount64();
        status = vipc_channel_wait_readable(client, DETECT_TIMEOUT_MS, &error);
        elapsed = GetTickCount64() - started;
        if (status != VIPC_ERR_PEER_CLOSED) {
            report_status("abrupt peer exit wait_readable", status, &error);
        }
        CHECK(status == VIPC_ERR_PEER_CLOSED,
            "wait_readable reports a peer process that exited");
        CHECK(error.phase == VIPC_PHASE_WAIT_READABLE,
            "abrupt peer exit phase is wait-readable");
    CHECK(elapsed < (ULONGLONG)PEER_CLOSE_BOUND_MS,
        "abrupt peer exit is detected without waiting for the deadline");
    CHECK(vipc_channel_is_open(client) == 0,
        "an abrupt peer exit closes the channel");
        status = vipc_channel_receive(
            client,
            &message,
            payload,
            (uint32_t)sizeof(payload),
            &payload_size,
            10u,
            &error);
        CHECK(status == VIPC_ERR_NOT_CONNECTED,
            "receive after an abrupt peer exit reports not-connected");
        printf(
            "  abrupt peer exit detected in %lu ms (platform_code=%lu)\n",
            (unsigned long)elapsed,
            (unsigned long)error.platform_code);

        if (client) vipc_channel_destroy(client);
        wait_result = WaitForSingleObject(process.hProcess, OP_TIMEOUT_MS);
        if (wait_result == WAIT_OBJECT_0) {
            (void)GetExitCodeProcess(process.hProcess, &exit_code);
            CHECK(exit_code == ABRUPT_EXIT_CODE, "abrupt peer exit code observed");
        } else {
            CHECK(0, "abrupt peer process exited");
            (void)TerminateProcess(process.hProcess, ABRUPT_EXIT_CODE);
            (void)WaitForSingleObject(process.hProcess, OP_TIMEOUT_MS);
        }
        if (process.hThread) (void)CloseHandle(process.hThread);
        if (process.hProcess) (void)CloseHandle(process.hProcess);
    }
}

typedef struct blocking_reader_context {
    vipc_channel *channel;
    HANDLE entered_event;
    vipc_status status;
    vipc_error error;
    int frame_intact;
} blocking_reader_context;

static DWORD WINAPI blocking_receive_main(LPVOID opaque) {
    blocking_reader_context *context = (blocking_reader_context *)opaque;
    vipc_message message;
    uint8_t payload[FRAME_PAYLOAD_BYTES];
    uint32_t payload_size = 0u;

    (void)SetEvent(context->entered_event);
    context->status = vipc_channel_receive(
        context->channel,
        &message,
        payload,
        (uint32_t)sizeof(payload),
        &payload_size,
        BLOCKING_TIMEOUT_MS,
        &context->error);
    if (context->status == VIPC_OK) {
        context->frame_intact = verify_frame(
            &message, payload, payload_size, OP_BUSY_PROBE, 1u) ? 1 : 0;
    }
    return 0u;
}

static DWORD WINAPI blocking_wait_main(LPVOID opaque) {
    blocking_reader_context *context = (blocking_reader_context *)opaque;

    (void)SetEvent(context->entered_event);
    context->status = vipc_channel_wait_readable(
        context->channel, BLOCKING_TIMEOUT_MS, &context->error);
    return 0u;
}

typedef enum {
    BLOCKER_RECEIVE = 0,
    BLOCKER_WAIT = 1
} blocker_kind;

typedef enum {
    PROBE_WAIT = 0,
    PROBE_RECEIVE = 1,
    PROBE_SEND = 2
} probe_kind;

/*
 * Evidence 3: a channel has exactly one reader-side slot. A blocked receive and
 * a readiness wait must both collide with it, and the writer side must not.
 *
 * Each case gets a private server and channel: a receive timeout poisons the
 * channel it happens on, so cases must not share one.
 */
static void test_reader_slot_case(
    const char *label,
    uint32_t endpoint_index,
    blocker_kind blocker,
    probe_kind probe) {
    char endpoint[96];
    peer_context peer;
    vipc_channel *client = NULL;
    vipc_error error;
    vipc_status status;
    vipc_message message;
    uint8_t payload[FRAME_PAYLOAD_BYTES];
    uint32_t payload_size = 0u;
    blocking_reader_context worker;
    HANDLE worker_thread = NULL;
    DWORD thread_id = 0u;
    DWORD wait_result;
    ULONGLONG started;
    ULONGLONG elapsed;
    uint32_t attempts;
    int slot_observed = 0;
    int expected_phase = (probe == PROBE_RECEIVE)
        ? (int)VIPC_PHASE_READ_HEADER
        : (int)VIPC_PHASE_WAIT_READABLE;

    make_endpoint(endpoint, sizeof(endpoint), "slot", endpoint_index);
    if (!peer_start(&peer, endpoint, OP_BUSY_PROBE, 1u, 0u)) {
        peer_finish(&peer);
        return;
    }
    if (!peer_connect(&peer, endpoint, &client)) {
        peer_finish(&peer);
        return;
    }

    ZeroMemory(&worker, sizeof(worker));
    worker.channel = client;
    worker.entered_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(worker.entered_event != NULL, label);
    if (!worker.entered_event) goto cleanup;

    worker_thread = CreateThread(
        NULL,
        0,
        blocker == BLOCKER_RECEIVE ? blocking_receive_main : blocking_wait_main,
        &worker,
        0,
        &thread_id);
    CHECK(worker_thread != NULL, label);
    if (!worker_thread) goto cleanup;
    (void)WaitForSingleObject(worker.entered_event, OP_TIMEOUT_MS);
    /*
     * Let the blocked call finish entering the transport before probing. The
     * blocker is a few instructions past its own "entered" signal, and a probe
     * that won the reader slot first would starve it instead of colliding with
     * it.
     */
    Sleep(BLOCKER_SETTLE_MS);

    /*
     * Phase 1: prove the reader slot is held, using only readiness timeouts,
     * which are non-destructive and therefore safe to repeat.
     */
    for (attempts = 0u; attempts < BUSY_PROBE_ATTEMPTS; ++attempts) {
        status = vipc_channel_wait_readable(client, 1u, &error);
        if (status == VIPC_ERR_BUSY) {
            slot_observed = 1;
            CHECK(error.phase == VIPC_PHASE_WAIT_READABLE,
                "reader slot is observable as busy from wait_readable");
            CHECK(error.platform_code == (uint32_t)ERROR_BUSY,
                "busy wait_readable reports ERROR_BUSY");
            break;
        }
        CHECK(status == VIPC_ERR_TIMEOUT,
            "reader slot probe only ever sees a timeout before the slot is held");
        if (status != VIPC_ERR_TIMEOUT) break;
    }
    CHECK(slot_observed, "the reader slot is provably held by the blocked call");
    if (!slot_observed) goto cleanup;

    /* Phase 2: the call under test, attempted while the slot is provably held. */
    if (probe == PROBE_WAIT) {
        started = GetTickCount64();
        status = vipc_channel_wait_readable(client, 1u, &error);
        elapsed = GetTickCount64() - started;
        if (status != VIPC_ERR_BUSY) {
            report_status(label, status, &error);
        }
        CHECK(status == VIPC_ERR_BUSY, label);
        CHECK(error.phase == (vipc_phase)expected_phase, label);
        CHECK(elapsed < (ULONGLONG)BUSY_REJECT_MAX_MS,
            "busy rejection is immediate instead of blocking");
    } else if (probe == PROBE_RECEIVE) {
        started = GetTickCount64();
        status = vipc_channel_receive(
            client,
            &message,
            payload,
            (uint32_t)sizeof(payload),
            &payload_size,
            RECEIVE_PROBE_TIMEOUT_MS,
            &error);
        elapsed = GetTickCount64() - started;
        if (status != VIPC_ERR_BUSY) {
            report_status(label, status, &error);
        }
        CHECK(status == VIPC_ERR_BUSY, label);
        CHECK(error.phase == (vipc_phase)expected_phase, label);
        CHECK(error.platform_code == (uint32_t)ERROR_BUSY,
            "busy receive reports ERROR_BUSY");
        CHECK(payload_size == 0u,
            "busy receive leaves the reported payload size cleared");
        CHECK(elapsed < (ULONGLONG)BUSY_REJECT_MAX_MS,
            "busy rejection is immediate instead of blocking");
    } else {
        build_frame(&message, OP_REPLY, UINT64_C(0xBEEF),
                    (uint32_t)sizeof(payload));
        fill_pattern(payload, (uint32_t)sizeof(payload), UINT64_C(0xBEEF));
        status = vipc_channel_send(client, &message, payload, OP_TIMEOUT_MS,
                                   &error);
        if (status != VIPC_OK) report_status(label, status, &error);
        CHECK(status == VIPC_OK, label);
    }
    CHECK(vipc_channel_is_open(client) == 1,
        "the busy case leaves the channel open");

    /* Phase 3: release the blocked call and let it finish on real data. */
    (void)ResetEvent(peer.sent_event);
    peer_request_send(&peer);
    (void)WaitForSingleObject(peer.sent_event, OP_TIMEOUT_MS);
    wait_result = WaitForSingleObject(worker_thread, OP_TIMEOUT_MS);
    CHECK(wait_result == WAIT_OBJECT_0, "blocked reader thread joined");
    (void)CloseHandle(worker_thread);
    worker_thread = NULL;
    if (worker.status != VIPC_OK) {
        report_status("blocked reader result", worker.status, &worker.error);
    }
    CHECK(worker.status == VIPC_OK,
        "blocked reader-side call completes after the busy probe");
    if (blocker == BLOCKER_RECEIVE) {
        CHECK(worker.frame_intact == 1,
            "the receive that held the slot consumed the intact frame");
    }

cleanup:
    /*
     * A blocked call must be released before the channel is destroyed, and the
     * peer's write must be known to have completed first, otherwise the write
     * races the close and reports a peer-closed failure that never happened.
     */
    if (worker_thread) {
        (void)ResetEvent(peer.sent_event);
        peer_request_send(&peer);
        (void)WaitForSingleObject(peer.sent_event, OP_TIMEOUT_MS);
        (void)WaitForSingleObject(worker_thread, OP_TIMEOUT_MS);
        (void)CloseHandle(worker_thread);
        worker_thread = NULL;
    }
    if (worker.entered_event) (void)CloseHandle(worker.entered_event);
    if (client) vipc_channel_destroy(client);
    peer_finish(&peer);
}

static void test_reader_slot_busy(void) {
    test_reader_slot_case(
        "wait_readable returns VIPC_ERR_BUSY while a receive holds the slot",
        4u, BLOCKER_RECEIVE, PROBE_WAIT);
    test_reader_slot_case(
        "receive returns VIPC_ERR_BUSY while wait_readable holds the slot",
        5u, BLOCKER_WAIT, PROBE_RECEIVE);
    test_reader_slot_case(
        "a second wait_readable returns VIPC_ERR_BUSY",
        6u, BLOCKER_WAIT, PROBE_WAIT);
    test_reader_slot_case(
        "send is unaffected while wait_readable holds the slot",
        7u, BLOCKER_WAIT, PROBE_SEND);
}

/*
 * Evidence 4: a persistent channel that stays idle for a long stretch, the way
 * a host-driven helper holds one connection between bursts of work, must still
 * carry a correct duplex exchange afterwards.
 */
static void test_long_idle_survival(uint32_t rounds, uint32_t idle_ms) {
    char endpoint[96];
    peer_context peer;
    vipc_channel *client = NULL;
    vipc_error error;
    vipc_status status;
    vipc_message message;
    uint8_t payload[FRAME_PAYLOAD_BYTES];
    uint8_t reply[FRAME_PAYLOAD_BYTES];
    uint32_t payload_size = 0u;
    uint32_t peer_pid;
    uint32_t peer_session;
    uint32_t i;
    ULONGLONG started = GetTickCount64();
    ULONGLONG idle_elapsed;
    int idle_ok = 1;

    make_endpoint(endpoint, sizeof(endpoint), "long", 8u);
    if (!peer_start(&peer, endpoint, OP_LONG_IDLE, 1u, 0u)) {
        peer_finish(&peer);
        return;
    }
    if (!peer_connect(&peer, endpoint, &client)) {
        peer_finish(&peer);
        return;
    }
    peer_pid = vipc_channel_peer_pid(client);
    peer_session = vipc_channel_peer_session_id(client);
    CHECK(peer_pid != 0u, "long-idle peer pid is recorded");
    CHECK(peer_session != 0u, "long-idle peer session is recorded");

    for (i = 0u; i < rounds; ++i) {
        ULONGLONG round_started = GetTickCount64();
        ULONGLONG round_elapsed;

        status = vipc_channel_wait_readable(client, idle_ms, &error);
        round_elapsed = GetTickCount64() - round_started;
        if (status != VIPC_ERR_TIMEOUT
            || error.phase != VIPC_PHASE_WAIT_READABLE
            || vipc_channel_is_open(client) != 1
            || vipc_channel_peer_pid(client) != peer_pid
            || vipc_channel_peer_session_id(client) != peer_session
            || round_elapsed + DEADLINE_TOLERANCE_MS < (ULONGLONG)idle_ms) {
            report_status("long-idle readiness timeout", status, &error);
            idle_ok = 0;
            break;
        }
    }
    idle_elapsed = GetTickCount64() - started;
    CHECK(idle_ok, "long-idle readiness timeouts stay non-destructive");
    CHECK(vipc_channel_is_open(client) == 1,
        "long-idle channel survives the idle stretch");
    CHECK(vipc_channel_peer_pid(client) == peer_pid,
        "long-idle channel keeps the same peer identity");
    printf(
        "  long idle: %lu rounds of %lu ms, %.2f s total, channel still open\n",
        (unsigned long)rounds,
        (unsigned long)idle_ms,
        (double)idle_elapsed / 1000.0);

    peer_request_send(&peer);
    status = vipc_channel_wait_readable(client, DETECT_TIMEOUT_MS, &error);
    if (status != VIPC_OK) report_status("long-idle wait_readable", status,
                                         &error);
    CHECK(status == VIPC_OK, "readiness observed after a long idle stretch");

    status = vipc_channel_receive(
        client,
        &message,
        payload,
        (uint32_t)sizeof(payload),
        &payload_size,
        OP_TIMEOUT_MS,
        &error);
    if (status != VIPC_OK) report_status("long-idle receive", status, &error);
    CHECK(status == VIPC_OK, "receive succeeds after a long idle stretch");
    CHECK(verify_frame(&message, payload, payload_size, OP_LONG_IDLE, 1u),
        "frame received after a long idle stretch is intact");

    build_frame(&message, OP_REPLY, UINT64_C(0x10AD),
                (uint32_t)sizeof(reply));
    fill_pattern(reply, (uint32_t)sizeof(reply), UINT64_C(0x10AD));
    status = vipc_channel_send(client, &message, reply, OP_TIMEOUT_MS, &error);
    if (status != VIPC_OK) report_status("long-idle send", status, &error);
    CHECK(status == VIPC_OK, "send succeeds after a long idle stretch");

    vipc_channel_destroy(client);
    peer_finish(&peer);
}

static int abrupt_peer_main(const char *endpoint, uint32_t lifetime_ms) {
    vipc_server *server = NULL;
    vipc_error error;

    if (vipc_server_create(endpoint, &server, &error) != VIPC_OK) return 2;

    /* Never accept and never tear the channel down: the client has to notice
     * this process vanishing while it is blocked in a readiness wait. */
    Sleep((DWORD)lifetime_ms);
    return (int)ABRUPT_EXIT_CODE;
}

int main(int argc, char **argv) {
    uint32_t idle_rounds = DEFAULT_IDLE_ROUNDS;
    uint32_t idle_ms = DEFAULT_IDLE_MS;
    uint32_t long_rounds = DEFAULT_LONG_IDLE_ROUNDS;
    uint32_t long_idle_ms = DEFAULT_LONG_IDLE_MS;
    uint32_t peer_lifetime_ms = ABRUPT_PEER_LIFETIME_MS;

    if (argc >= 2 && strcmp(argv[1], "--abrupt-peer") == 0) {
        if (argc != 4 || !parse_u32(argv[3], &peer_lifetime_ms)) return 2;
        return abrupt_peer_main(argv[2], peer_lifetime_ms);
    }
    if (argc >= 2 && !parse_u32(argv[1], &idle_rounds)) return 2;
    if (argc >= 3 && !parse_u32(argv[2], &idle_ms)) return 2;
    if (argc >= 4 && !parse_u32(argv[3], &long_rounds)) return 2;
    if (argc >= 5 && !parse_u32(argv[4], &long_idle_ms)) return 2;
    if (argc > 5
        || idle_ms > MAX_IDLE_MS || long_idle_ms > MAX_IDLE_MS
        || idle_rounds > MAX_ROUNDS || long_rounds > MAX_ROUNDS) {
        fprintf(
            stderr,
            "usage: vectoripc_readiness_stress [idle-rounds] [idle-ms] "
            "[long-idle-rounds] [long-idle-ms]\n");
        return 2;
    }

    printf("VectorIPC readiness stress\n");
    test_repeated_idle_timeouts(idle_rounds, idle_ms);
    test_peer_close_detection();
    test_reader_slot_busy();
    test_long_idle_survival(long_rounds, long_idle_ms);

    if (g_failures != 0) {
        fprintf(stderr, "VectorIPC readiness stress: %d failure(s)\n",
                g_failures);
        return 1;
    }
    printf("VectorIPC readiness stress: PASS\n");
    return 0;
}

#else

int main(void) {
    printf("VectorIPC readiness stress: SKIP (Windows only)\n");
    return 0;
}

#endif
