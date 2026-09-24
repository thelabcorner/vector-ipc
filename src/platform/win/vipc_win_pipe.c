#include "vectoripc/vipc.h"

#include <windows.h>

#define VIPC_WIN_PIPE_NAME_CAP 256u
#define VIPC_WIN_PIPE_PREFIX_CHARS 19u
#define VIPC_WIN_SESSION_ID_MAX_CHARS 10u

#if VIPC_WIN_PIPE_NAME_CAP < \
    (VIPC_WIN_PIPE_PREFIX_CHARS \
        + VIPC_WIN_SESSION_ID_MAX_CHARS \
        + 1u \
        + VIPC_ENDPOINT_MAX \
        + 1u)
#error "VIPC_WIN_PIPE_NAME_CAP is too small for the maximum VectorIPC endpoint"
#endif
#define VIPC_CONNECT_POLL_MS 25u
#define VIPC_CONNECT_SLEEP_MS 2u
#define VIPC_DRAIN_CHUNK 4096u

struct vipc_channel {
    HANDLE pipe;
    HANDLE read_event;
    HANDLE write_event;
    OVERLAPPED read_overlapped;
    OVERLAPPED write_overlapped;
    volatile LONG read_busy;
    volatile LONG write_busy;
    volatile LONG read_pending;
    volatile LONG write_pending;
    volatile LONG poisoned;
    int server_side;
    uint32_t peer_pid;
    uint32_t peer_session_id;
};

struct vipc_server {
    WCHAR pipe_name[VIPC_WIN_PIPE_NAME_CAP];
    SECURITY_ATTRIBUTES security_attributes;
    PSECURITY_DESCRIPTOR security_descriptor;
    PACL access_control_list;
    HANDLE listen_pipe;
    HANDLE accept_event;
    OVERLAPPED accept_overlapped;
    volatile LONG accept_busy;
    volatile LONG accept_pending;
    volatile LONG poisoned;
    int ever_accepted;
};

static vipc_status set_error(
    vipc_error *error,
    vipc_status status,
    vipc_phase phase,
    DWORD platform_code) {
    if (error) {
        error->status = status;
        error->phase = phase;
        error->platform_code = (uint32_t)platform_code;
    }
    return status;
}

static int timeout_is_valid(uint32_t timeout_ms) {
    return timeout_ms <= VIPC_TIMEOUT_MAX_MS;
}

static ULONGLONG deadline_from_timeout(uint32_t timeout_ms) {
    ULONGLONG now = GetTickCount64();
    ULONGLONG deadline = now + (ULONGLONG)timeout_ms;
    if (deadline < now) return ~(ULONGLONG)0;
    return deadline;
}

static DWORD remaining_timeout(ULONGLONG deadline) {
    ULONGLONG now = GetTickCount64();
    ULONGLONG remaining;
    if (now >= deadline) return 0;
    remaining = deadline - now;
    if (remaining >= (ULONGLONG)INFINITE) return INFINITE - 1u;
    return (DWORD)remaining;
}

static DWORD min_dword(DWORD a, DWORD b) {
    return a < b ? a : b;
}

static vipc_status map_protocol_status(
    vipc_protocol_status protocol_status,
    vipc_phase phase,
    vipc_error *error) {
    switch (protocol_status) {
        case VIPC_PROTOCOL_OK:
            return VIPC_OK;
        case VIPC_PROTOCOL_BAD_MAGIC:
            return set_error(error, VIPC_ERR_PROTOCOL_MAGIC, phase, 0);
        case VIPC_PROTOCOL_BAD_VERSION:
            return set_error(error, VIPC_ERR_PROTOCOL_VERSION, phase, 0);
        case VIPC_PROTOCOL_PAYLOAD_TOO_LARGE:
            return set_error(error, VIPC_ERR_PAYLOAD_TOO_LARGE, phase, 0);
        case VIPC_PROTOCOL_BAD_ARGUMENT:
            return set_error(error, VIPC_ERR_INVALID_ARGUMENT, phase, 0);
        default:
            return set_error(error, VIPC_ERR_PROTOCOL_INVALID, phase, 0);
    }
}

static vipc_status map_io_error(DWORD winerr, vipc_phase phase, vipc_error *error) {
    switch (winerr) {
        case ERROR_BROKEN_PIPE:
        case ERROR_PIPE_NOT_CONNECTED:
        case ERROR_NO_DATA:
            return set_error(error, VIPC_ERR_PEER_CLOSED, phase, winerr);
        case ERROR_ACCESS_DENIED:
            return set_error(error, VIPC_ERR_ACCESS_DENIED, phase, winerr);
        case ERROR_OPERATION_ABORTED:
            return set_error(error, VIPC_ERR_IO, phase, winerr);
        default:
            return set_error(error, VIPC_ERR_IO, phase, winerr);
    }
}

static HANDLE take_pipe_handle(vipc_channel *channel) {
    return (HANDLE)InterlockedExchangePointer(
        (PVOID volatile *)&channel->pipe,
        NULL);
}

/*
 * Graceful close for an accepted channel.
 *
 * Accepted VectorIPC pipe instances are never reused. Do not call
 * DisconnectNamedPipe here: it discards unread server->client bytes. A plain
 * CloseHandle lets already-buffered bytes remain readable by the client while
 * the server end enters the closing state.
 */
static void close_channel_pipe(vipc_channel *channel) {
    HANDLE pipe;
    if (!channel) return;
    pipe = take_pipe_handle(channel);
    if (!pipe || pipe == INVALID_HANDLE_VALUE) return;
    (void)CloseHandle(pipe);
}

/* Forced close for poisoned channels, where discarding unread data is desired. */
static void abort_channel_pipe(vipc_channel *channel) {
    HANDLE pipe;
    if (!channel) return;
    pipe = take_pipe_handle(channel);
    if (!pipe || pipe == INVALID_HANDLE_VALUE) return;
    if (channel->server_side) {
        (void)DisconnectNamedPipe(pipe);
    }
    (void)CloseHandle(pipe);
}

static void poison_channel(vipc_channel *channel) {
    if (!channel) return;
    (void)InterlockedExchange(&channel->poisoned, 1);
    abort_channel_pipe(channel);
}

static int channel_has_unsettled_io(vipc_channel *channel) {
    DWORD wait_result;

    if (InterlockedCompareExchange(&channel->read_pending, 0, 0) != 0) {
        wait_result = WaitForSingleObject(channel->read_event, 0);
        if (wait_result == WAIT_OBJECT_0) {
            (void)InterlockedExchange(&channel->read_pending, 0);
        }
    }

    if (InterlockedCompareExchange(&channel->write_pending, 0, 0) != 0) {
        wait_result = WaitForSingleObject(channel->write_event, 0);
        if (wait_result == WAIT_OBJECT_0) {
            (void)InterlockedExchange(&channel->write_pending, 0);
        }
    }

    return InterlockedCompareExchange(&channel->read_pending, 0, 0) != 0
        || InterlockedCompareExchange(&channel->write_pending, 0, 0) != 0;
}

static int append_wide_literal(
    WCHAR *out,
    size_t capacity,
    size_t *used,
    const WCHAR *text) {
    size_t i = 0;
    while (text[i] != L'\0') {
        if (*used + 1u >= capacity) return 0;
        out[*used] = text[i];
        *used += 1u;
        ++i;
    }
    out[*used] = L'\0';
    return 1;
}

static int append_ascii_token(
    WCHAR *out,
    size_t capacity,
    size_t *used,
    const char *text) {
    size_t i = 0;
    while (text[i] != '\0') {
        if (*used + 1u >= capacity) return 0;
        out[*used] = (WCHAR)(unsigned char)text[i];
        *used += 1u;
        ++i;
    }
    out[*used] = L'\0';
    return 1;
}

static int append_u32_decimal(
    WCHAR *out,
    size_t capacity,
    size_t *used,
    uint32_t value) {
    WCHAR reversed[10];
    size_t count = 0;
    size_t i;

    if (value == 0u) {
        if (*used + 1u >= capacity) return 0;
        out[*used] = L'0';
        *used += 1u;
        out[*used] = L'\0';
        return 1;
    }

    while (value != 0u) {
        reversed[count++] = (WCHAR)(L'0' + (value % 10u));
        value /= 10u;
    }

    if (*used + count >= capacity) return 0;
    for (i = 0; i < count; ++i) {
        out[*used + i] = reversed[count - i - 1u];
    }
    *used += count;
    out[*used] = L'\0';
    return 1;
}

static vipc_status build_pipe_name(
    const char *endpoint,
    WCHAR out_name[VIPC_WIN_PIPE_NAME_CAP],
    vipc_error *error) {
    DWORD session_id = 0;
    size_t used = 0;

    if (!vipc_endpoint_is_valid(endpoint)) {
        return set_error(error, VIPC_ERR_INVALID_ENDPOINT, VIPC_PHASE_ENDPOINT, 0);
    }

    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) {
        return set_error(
            error,
            VIPC_ERR_INTERNAL,
            VIPC_PHASE_ENDPOINT,
            GetLastError());
    }

    out_name[0] = L'\0';
    if (!append_wide_literal(
            out_name,
            VIPC_WIN_PIPE_NAME_CAP,
            &used,
            L"\\\\.\\pipe\\VectorIPC.")
        || !append_u32_decimal(
            out_name,
            VIPC_WIN_PIPE_NAME_CAP,
            &used,
            (uint32_t)session_id)
        || !append_wide_literal(
            out_name,
            VIPC_WIN_PIPE_NAME_CAP,
            &used,
            L".")
        || !append_ascii_token(
            out_name,
            VIPC_WIN_PIPE_NAME_CAP,
            &used,
            endpoint)) {
        return set_error(error, VIPC_ERR_INVALID_ENDPOINT, VIPC_PHASE_ENDPOINT, 0);
    }

    return VIPC_OK;
}

static void free_server_security(vipc_server *server) {
    HANDLE heap;
    if (!server) return;
    heap = GetProcessHeap();
    if (server->access_control_list) {
        (void)HeapFree(heap, 0, server->access_control_list);
        server->access_control_list = NULL;
    }
    if (server->security_descriptor) {
        (void)HeapFree(heap, 0, server->security_descriptor);
        server->security_descriptor = NULL;
    }
    server->security_attributes.lpSecurityDescriptor = NULL;
}

static vipc_status build_current_user_security(
    vipc_server *server,
    vipc_error *error) {
    HANDLE token = NULL;
    HANDLE heap = GetProcessHeap();
    TOKEN_USER *token_user = NULL;
    DWORD token_bytes = 0;
    DWORD sid_bytes;
    DWORD acl_bytes;
    DWORD winerr;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return set_error(
            error,
            VIPC_ERR_ACCESS_DENIED,
            VIPC_PHASE_SECURITY,
            GetLastError());
    }

    (void)GetTokenInformation(token, TokenUser, NULL, 0, &token_bytes);
    winerr = GetLastError();
    if (token_bytes == 0u || winerr != ERROR_INSUFFICIENT_BUFFER) {
        (void)CloseHandle(token);
        return set_error(
            error,
            VIPC_ERR_INTERNAL,
            VIPC_PHASE_SECURITY,
            winerr);
    }

    token_user = (TOKEN_USER *)HeapAlloc(heap, HEAP_ZERO_MEMORY, token_bytes);
    if (!token_user) {
        (void)CloseHandle(token);
        return set_error(
            error,
            VIPC_ERR_OUT_OF_MEMORY,
            VIPC_PHASE_SECURITY,
            ERROR_OUTOFMEMORY);
    }

    if (!GetTokenInformation(
            token,
            TokenUser,
            token_user,
            token_bytes,
            &token_bytes)) {
        winerr = GetLastError();
        (void)HeapFree(heap, 0, token_user);
        (void)CloseHandle(token);
        return set_error(
            error,
            VIPC_ERR_INTERNAL,
            VIPC_PHASE_SECURITY,
            winerr);
    }

    sid_bytes = GetLengthSid(token_user->User.Sid);
    acl_bytes = (DWORD)sizeof(ACL)
        + (DWORD)sizeof(ACCESS_ALLOWED_ACE)
        - (DWORD)sizeof(DWORD)
        + sid_bytes;

    server->security_descriptor = (PSECURITY_DESCRIPTOR)HeapAlloc(
        heap,
        HEAP_ZERO_MEMORY,
        SECURITY_DESCRIPTOR_MIN_LENGTH);
    server->access_control_list = (PACL)HeapAlloc(
        heap,
        HEAP_ZERO_MEMORY,
        acl_bytes);

    if (!server->security_descriptor || !server->access_control_list) {
        (void)HeapFree(heap, 0, token_user);
        (void)CloseHandle(token);
        free_server_security(server);
        return set_error(
            error,
            VIPC_ERR_OUT_OF_MEMORY,
            VIPC_PHASE_SECURITY,
            ERROR_OUTOFMEMORY);
    }

    if (!InitializeSecurityDescriptor(
            server->security_descriptor,
            SECURITY_DESCRIPTOR_REVISION)
        || !InitializeAcl(
            server->access_control_list,
            acl_bytes,
            ACL_REVISION)
        || !AddAccessAllowedAce(
            server->access_control_list,
            ACL_REVISION,
            GENERIC_ALL,
            token_user->User.Sid)
        || !SetSecurityDescriptorDacl(
            server->security_descriptor,
            TRUE,
            server->access_control_list,
            FALSE)) {
        winerr = GetLastError();
        (void)HeapFree(heap, 0, token_user);
        (void)CloseHandle(token);
        free_server_security(server);
        return set_error(
            error,
            VIPC_ERR_INTERNAL,
            VIPC_PHASE_SECURITY,
            winerr);
    }

    server->security_attributes.nLength = (DWORD)sizeof(SECURITY_ATTRIBUTES);
    server->security_attributes.lpSecurityDescriptor =
        server->security_descriptor;
    server->security_attributes.bInheritHandle = FALSE;

    (void)HeapFree(heap, 0, token_user);
    (void)CloseHandle(token);
    return VIPC_OK;
}

static vipc_status create_listen_pipe(
    vipc_server *server,
    int first_instance,
    vipc_error *error) {
    DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
    DWORD pipe_mode = PIPE_TYPE_BYTE
        | PIPE_READMODE_BYTE
        | PIPE_WAIT
        | PIPE_REJECT_REMOTE_CLIENTS;
    HANDLE pipe;
    DWORD winerr;

    if (first_instance) open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;

    pipe = CreateNamedPipeW(
        server->pipe_name,
        open_mode,
        pipe_mode,
        PIPE_UNLIMITED_INSTANCES,
        VIPC_DEFAULT_PIPE_BUFFER_BYTES,
        VIPC_DEFAULT_PIPE_BUFFER_BYTES,
        0,
        &server->security_attributes);

    if (pipe == INVALID_HANDLE_VALUE) {
        winerr = GetLastError();
        if (winerr == ERROR_ACCESS_DENIED && first_instance) {
            return set_error(
                error,
                VIPC_ERR_ENDPOINT_IN_USE,
                VIPC_PHASE_LISTEN,
                winerr);
        }
        if (winerr == ERROR_ACCESS_DENIED) {
            return set_error(
                error,
                VIPC_ERR_ACCESS_DENIED,
                VIPC_PHASE_LISTEN,
                winerr);
        }
        return set_error(error, VIPC_ERR_IO, VIPC_PHASE_LISTEN, winerr);
    }

    server->listen_pipe = pipe;
    return VIPC_OK;
}

static vipc_status channel_from_pipe(
    HANDLE pipe,
    int server_side,
    vipc_channel **out_channel,
    vipc_error *error) {
    HANDLE heap = GetProcessHeap();
    vipc_channel *channel;
    ULONG peer_pid = 0;
    ULONG peer_session = 0;
    DWORD local_session = 0;
    DWORD read_event_error = ERROR_SUCCESS;
    DWORD write_event_error = ERROR_SUCCESS;
    DWORD peer_pid_error = ERROR_SUCCESS;
    DWORD peer_session_error = ERROR_SUCCESS;
    DWORD local_session_error = ERROR_SUCCESS;
    BOOL ok_pid;
    BOOL ok_session;
    BOOL ok_local_session;

    channel = (vipc_channel *)HeapAlloc(
        heap,
        HEAP_ZERO_MEMORY,
        sizeof(vipc_channel));
    if (!channel) {
        (void)CloseHandle(pipe);
        return set_error(
            error,
            VIPC_ERR_OUT_OF_MEMORY,
            server_side ? VIPC_PHASE_ACCEPT : VIPC_PHASE_CONNECT,
            ERROR_OUTOFMEMORY);
    }

    channel->pipe = pipe;
    channel->server_side = server_side;
    channel->read_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!channel->read_event) read_event_error = GetLastError();
    channel->write_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!channel->write_event) write_event_error = GetLastError();

    if (!channel->read_event || !channel->write_event) {
        DWORD winerr = !channel->read_event
            ? read_event_error
            : write_event_error;
        if (channel->read_event) (void)CloseHandle(channel->read_event);
        if (channel->write_event) (void)CloseHandle(channel->write_event);
        (void)CloseHandle(pipe);
        (void)HeapFree(heap, 0, channel);
        return set_error(
            error,
            VIPC_ERR_INTERNAL,
            server_side ? VIPC_PHASE_ACCEPT : VIPC_PHASE_CONNECT,
            winerr);
    }

    if (server_side) {
        ok_pid = GetNamedPipeClientProcessId(pipe, &peer_pid);
        if (!ok_pid) peer_pid_error = GetLastError();
        ok_session = GetNamedPipeClientSessionId(pipe, &peer_session);
        if (!ok_session) peer_session_error = GetLastError();
    } else {
        ok_pid = GetNamedPipeServerProcessId(pipe, &peer_pid);
        if (!ok_pid) peer_pid_error = GetLastError();
        ok_session = GetNamedPipeServerSessionId(pipe, &peer_session);
        if (!ok_session) peer_session_error = GetLastError();
    }

    ok_local_session = ProcessIdToSessionId(
        GetCurrentProcessId(),
        &local_session);
    if (!ok_local_session) local_session_error = GetLastError();

    if (!ok_local_session || !ok_pid || !ok_session) {
        DWORD winerr = !ok_local_session
            ? local_session_error
            : (!ok_pid ? peer_pid_error : peer_session_error);
        vipc_phase phase = server_side
            ? VIPC_PHASE_ACCEPT
            : VIPC_PHASE_CONNECT;
        (void)CloseHandle(channel->read_event);
        (void)CloseHandle(channel->write_event);
        (void)CloseHandle(pipe);
        (void)HeapFree(heap, 0, channel);
        if (!ok_local_session) {
            return set_error(error, VIPC_ERR_IO, phase, winerr);
        }
        return map_io_error(winerr, phase, error);
    }

    if (peer_session != (ULONG)local_session) {
        (void)CloseHandle(channel->read_event);
        (void)CloseHandle(channel->write_event);
        (void)CloseHandle(pipe);
        (void)HeapFree(heap, 0, channel);
        return set_error(
            error,
            VIPC_ERR_ACCESS_DENIED,
            server_side ? VIPC_PHASE_ACCEPT : VIPC_PHASE_CONNECT,
            ERROR_ACCESS_DENIED);
    }

    channel->peer_pid = (uint32_t)peer_pid;
    channel->peer_session_id = (uint32_t)peer_session;
    *out_channel = channel;
    return VIPC_OK;
}

static vipc_status finish_pending_io(
    vipc_channel *channel,
    OVERLAPPED *overlapped,
    volatile LONG *pending,
    ULONGLONG deadline,
    DWORD *bytes_done,
    vipc_phase phase,
    vipc_error *error) {
    DWORD wait_result;
    DWORD wait_ms = remaining_timeout(deadline);
    DWORD winerr;

    wait_result = WaitForSingleObject(overlapped->hEvent, wait_ms);
    if (wait_result == WAIT_OBJECT_0) {
        if (!GetOverlappedResult(channel->pipe, overlapped, bytes_done, FALSE)) {
            winerr = GetLastError();
            (void)InterlockedExchange(pending, 0);
            return map_io_error(winerr, phase, error);
        }
        (void)InterlockedExchange(pending, 0);
        return VIPC_OK;
    }

    if (wait_result != WAIT_TIMEOUT) {
        winerr = GetLastError();
        (void)CancelIoEx(channel->pipe, overlapped);
        (void)InterlockedExchange(&channel->poisoned, 1);
        abort_channel_pipe(channel);
        return set_error(error, VIPC_ERR_IO, phase, winerr);
    }

    /*
     * The operation deadline is exhausted. Cancel only this OVERLAPPED request.
     * Microsoft requires the OVERLAPPED storage to remain valid until the
     * cancellation itself completes, so allow a tiny bounded settle window.
     */
    if (!CancelIoEx(channel->pipe, overlapped)) {
        winerr = GetLastError();
        if (winerr != ERROR_NOT_FOUND) {
            (void)InterlockedExchange(&channel->poisoned, 1);
        }
    }

    wait_result = WaitForSingleObject(
        overlapped->hEvent,
        (DWORD)VIPC_CANCEL_SETTLE_MS);
    if (wait_result == WAIT_OBJECT_0) {
        DWORD ignored = 0;
        (void)GetOverlappedResult(channel->pipe, overlapped, &ignored, FALSE);
        (void)InterlockedExchange(pending, 0);
        poison_channel(channel);
        return set_error(
            error,
            VIPC_ERR_TIMEOUT,
            phase,
            ERROR_SEM_TIMEOUT);
    }

    /*
     * Catastrophic cancellation failure: close the channel and deliberately
     * leave the pending OVERLAPPED storage attached to the channel object. Its
     * destructor will quarantine (leak) the tiny object if the kernel still has
     * not signaled completion. A bounded host stall is preferable to a UAF.
     */
    (void)InterlockedExchange(&channel->poisoned, 1);
    abort_channel_pipe(channel);
    return set_error(
        error,
        VIPC_ERR_CANCEL_STUCK,
        VIPC_PHASE_CANCEL,
        ERROR_SEM_TIMEOUT);
}

static vipc_status io_once(
    vipc_channel *channel,
    int is_read,
    void *buffer,
    DWORD length,
    ULONGLONG deadline,
    DWORD *bytes_done,
    vipc_phase phase,
    vipc_error *error) {
    OVERLAPPED *overlapped =
        is_read ? &channel->read_overlapped : &channel->write_overlapped;
    HANDLE event_handle =
        is_read ? channel->read_event : channel->write_event;
    volatile LONG *pending =
        is_read ? &channel->read_pending : &channel->write_pending;
    BOOL ok;
    DWORD winerr;

    ZeroMemory(overlapped, sizeof(*overlapped));
    overlapped->hEvent = event_handle;
    (void)ResetEvent(event_handle);
    *bytes_done = 0;
    (void)InterlockedExchange(pending, 0);

    if (is_read) {
        ok = ReadFile(
            channel->pipe,
            buffer,
            length,
            bytes_done,
            overlapped);
    } else {
        ok = WriteFile(
            channel->pipe,
            buffer,
            length,
            bytes_done,
            overlapped);
    }

    if (ok) return VIPC_OK;

    winerr = GetLastError();
    if (winerr != ERROR_IO_PENDING) {
        return map_io_error(winerr, phase, error);
    }

    (void)InterlockedExchange(pending, 1);
    return finish_pending_io(
        channel,
        overlapped,
        pending,
        deadline,
        bytes_done,
        phase,
        error);
}

static vipc_status io_exact(
    vipc_channel *channel,
    int is_read,
    void *buffer,
    uint32_t length,
    ULONGLONG deadline,
    vipc_phase phase,
    vipc_error *error) {
    uint8_t *bytes = (uint8_t *)buffer;
    uint32_t offset = 0;

    while (offset < length) {
        DWORD completed = 0;
        DWORD remaining = (DWORD)(length - offset);
        vipc_status status = io_once(
            channel,
            is_read,
            bytes + offset,
            remaining,
            deadline,
            &completed,
            phase,
            error);
        if (status != VIPC_OK) return status;
        if (completed == 0u) {
            return set_error(
                error,
                is_read ? VIPC_ERR_PEER_CLOSED : VIPC_ERR_IO,
                phase,
                is_read ? ERROR_BROKEN_PIPE : ERROR_WRITE_FAULT);
        }
        offset += (uint32_t)completed;
    }

    return VIPC_OK;
}

vipc_status vipc_server_create(
    const char *endpoint,
    vipc_server **out_server,
    vipc_error *error) {
    HANDLE heap = GetProcessHeap();
    vipc_server *server;
    vipc_status status;

    vipc_error_clear(error);
    if (!out_server) {
        return set_error(
            error,
            VIPC_ERR_INVALID_ARGUMENT,
            VIPC_PHASE_LISTEN,
            ERROR_INVALID_PARAMETER);
    }
    *out_server = NULL;

    server = (vipc_server *)HeapAlloc(
        heap,
        HEAP_ZERO_MEMORY,
        sizeof(vipc_server));
    if (!server) {
        return set_error(
            error,
            VIPC_ERR_OUT_OF_MEMORY,
            VIPC_PHASE_LISTEN,
            ERROR_OUTOFMEMORY);
    }

    status = build_pipe_name(endpoint, server->pipe_name, error);
    if (status != VIPC_OK) {
        (void)HeapFree(heap, 0, server);
        return status;
    }

    status = build_current_user_security(server, error);
    if (status != VIPC_OK) {
        (void)HeapFree(heap, 0, server);
        return status;
    }

    server->accept_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!server->accept_event) {
        DWORD winerr = GetLastError();
        free_server_security(server);
        (void)HeapFree(heap, 0, server);
        return set_error(
            error,
            VIPC_ERR_INTERNAL,
            VIPC_PHASE_LISTEN,
            winerr);
    }

    status = create_listen_pipe(server, 1, error);
    if (status != VIPC_OK) {
        (void)CloseHandle(server->accept_event);
        free_server_security(server);
        (void)HeapFree(heap, 0, server);
        return status;
    }

    *out_server = server;
    return VIPC_OK;
}

void vipc_server_destroy(vipc_server *server) {
    HANDLE heap;
    LONG accept_busy;
    LONG accept_pending;

    if (!server) return;
    heap = GetProcessHeap();
    (void)InterlockedExchange(&server->poisoned, 1);

    if (server->listen_pipe && server->listen_pipe != INVALID_HANDLE_VALUE) {
        if (InterlockedCompareExchange(&server->accept_pending, 0, 0) != 0) {
            (void)CancelIoEx(server->listen_pipe, &server->accept_overlapped);
        }
        (void)CloseHandle(server->listen_pipe);
        server->listen_pipe = NULL;
    }

    accept_busy = InterlockedCompareExchange(&server->accept_busy, 0, 0);
    accept_pending = InterlockedCompareExchange(&server->accept_pending, 0, 0);
    if (accept_pending != 0
        && WaitForSingleObject(server->accept_event, 0) == WAIT_OBJECT_0) {
        (void)InterlockedExchange(&server->accept_pending, 0);
        accept_pending = 0;
    }

    if (accept_busy != 0 || accept_pending != 0) {
        /* See channel destructor: preserve OVERLAPPED storage rather than UAF. */
        return;
    }

    if (server->accept_event) (void)CloseHandle(server->accept_event);
    free_server_security(server);
    (void)HeapFree(heap, 0, server);
}

static vipc_status reset_listener_instance(
    vipc_server *server,
    vipc_error *error) {
    if (server->listen_pipe && server->listen_pipe != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(server->listen_pipe);
        server->listen_pipe = NULL;
    }
    return create_listen_pipe(server, server->ever_accepted ? 0 : 1, error);
}

vipc_status vipc_server_accept(
    vipc_server *server,
    uint32_t timeout_ms,
    vipc_channel **out_channel,
    vipc_error *error) {
    BOOL connected;
    DWORD winerr;
    DWORD wait_result;
    ULONGLONG deadline;
    vipc_status status;
    HANDLE accepted_pipe;
    vipc_channel *channel = NULL;

    vipc_error_clear(error);
    if (!server || !out_channel || !timeout_is_valid(timeout_ms)) {
        return set_error(
            error,
            VIPC_ERR_INVALID_ARGUMENT,
            VIPC_PHASE_ACCEPT,
            ERROR_INVALID_PARAMETER);
    }
    *out_channel = NULL;

    if (InterlockedCompareExchange(&server->poisoned, 0, 0) != 0
        || !server->listen_pipe) {
        return set_error(
            error,
            VIPC_ERR_NOT_CONNECTED,
            VIPC_PHASE_ACCEPT,
            ERROR_INVALID_HANDLE);
    }

    if (InterlockedCompareExchange(&server->accept_busy, 1, 0) != 0) {
        return set_error(error, VIPC_ERR_BUSY, VIPC_PHASE_ACCEPT, ERROR_BUSY);
    }

    deadline = deadline_from_timeout(timeout_ms);
    ZeroMemory(&server->accept_overlapped, sizeof(server->accept_overlapped));
    server->accept_overlapped.hEvent = server->accept_event;
    (void)ResetEvent(server->accept_event);
    (void)InterlockedExchange(&server->accept_pending, 0);

    connected = ConnectNamedPipe(
        server->listen_pipe,
        &server->accept_overlapped);
    if (!connected) {
        winerr = GetLastError();
        if (winerr == ERROR_PIPE_CONNECTED) {
            connected = TRUE;
        } else if (winerr == ERROR_IO_PENDING) {
            (void)InterlockedExchange(&server->accept_pending, 1);
            wait_result = WaitForSingleObject(
                server->accept_event,
                remaining_timeout(deadline));

            if (wait_result == WAIT_OBJECT_0) {
                DWORD ignored = 0;
                if (!GetOverlappedResult(
                        server->listen_pipe,
                        &server->accept_overlapped,
                        &ignored,
                        FALSE)) {
                    vipc_status recovery_status;
                    vipc_error recovery_error;
                    winerr = GetLastError();
                    (void)InterlockedExchange(&server->accept_pending, 0);
                    recovery_status = reset_listener_instance(
                        server,
                        &recovery_error);
                    (void)InterlockedExchange(&server->accept_busy, 0);
                    if (recovery_status != VIPC_OK) {
                        (void)InterlockedExchange(&server->poisoned, 1);
                        if (error) *error = recovery_error;
                        return recovery_status;
                    }
                    return map_io_error(winerr, VIPC_PHASE_ACCEPT, error);
                }
                (void)InterlockedExchange(&server->accept_pending, 0);
                connected = TRUE;
            } else if (wait_result == WAIT_TIMEOUT) {
                (void)CancelIoEx(
                    server->listen_pipe,
                    &server->accept_overlapped);
                wait_result = WaitForSingleObject(
                    server->accept_event,
                    (DWORD)VIPC_CANCEL_SETTLE_MS);
                if (wait_result == WAIT_OBJECT_0) {
                    DWORD ignored = 0;
                    (void)GetOverlappedResult(
                        server->listen_pipe,
                        &server->accept_overlapped,
                        &ignored,
                        FALSE);
                    (void)InterlockedExchange(&server->accept_pending, 0);
                    status = reset_listener_instance(server, error);
                    (void)InterlockedExchange(&server->accept_busy, 0);
                    if (status != VIPC_OK) {
                        (void)InterlockedExchange(&server->poisoned, 1);
                        return status;
                    }
                    return set_error(
                        error,
                        VIPC_ERR_TIMEOUT,
                        VIPC_PHASE_ACCEPT,
                        ERROR_SEM_TIMEOUT);
                }

                (void)InterlockedExchange(&server->poisoned, 1);
                if (server->listen_pipe) {
                    (void)CloseHandle(server->listen_pipe);
                    server->listen_pipe = NULL;
                }
                (void)InterlockedExchange(&server->accept_busy, 0);
                return set_error(
                    error,
                    VIPC_ERR_CANCEL_STUCK,
                    VIPC_PHASE_CANCEL,
                    ERROR_SEM_TIMEOUT);
            } else {
                winerr = GetLastError();
                (void)CancelIoEx(
                    server->listen_pipe,
                    &server->accept_overlapped);
                (void)InterlockedExchange(&server->poisoned, 1);
                (void)InterlockedExchange(&server->accept_busy, 0);
                return set_error(error, VIPC_ERR_IO, VIPC_PHASE_ACCEPT, winerr);
            }
        } else {
            vipc_status recovery_status;
            vipc_error recovery_error;
            recovery_status = reset_listener_instance(
                server,
                &recovery_error);
            (void)InterlockedExchange(&server->accept_busy, 0);
            if (recovery_status != VIPC_OK) {
                (void)InterlockedExchange(&server->poisoned, 1);
                if (error) *error = recovery_error;
                return recovery_status;
            }
            return map_io_error(winerr, VIPC_PHASE_ACCEPT, error);
        }
    }

    if (!connected) {
        (void)InterlockedExchange(&server->accept_busy, 0);
        return set_error(
            error,
            VIPC_ERR_INTERNAL,
            VIPC_PHASE_ACCEPT,
            ERROR_PIPE_NOT_CONNECTED);
    }

    accepted_pipe = server->listen_pipe;
    server->listen_pipe = NULL;

    status = channel_from_pipe(accepted_pipe, 1, &channel, error);
    if (status != VIPC_OK) {
        vipc_status recovery_status;
        vipc_error recovery_error;
        server->ever_accepted = 1;
        recovery_status = create_listen_pipe(
            server,
            0,
            &recovery_error);
        (void)InterlockedExchange(&server->accept_busy, 0);
        if (recovery_status != VIPC_OK) {
            (void)InterlockedExchange(&server->poisoned, 1);
            if (error) *error = recovery_error;
            return recovery_status;
        }
        return status;
    }

    server->ever_accepted = 1;
    status = create_listen_pipe(server, 0, error);
    if (status != VIPC_OK) {
        vipc_channel_destroy(channel);
        (void)InterlockedExchange(&server->poisoned, 1);
        (void)InterlockedExchange(&server->accept_busy, 0);
        return status;
    }

    *out_channel = channel;
    (void)InterlockedExchange(&server->accept_busy, 0);
    return VIPC_OK;
}

vipc_status vipc_client_connect(
    const char *endpoint,
    uint32_t timeout_ms,
    vipc_channel **out_channel,
    vipc_error *error) {
    WCHAR pipe_name[VIPC_WIN_PIPE_NAME_CAP];
    ULONGLONG deadline;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    DWORD winerr = ERROR_FILE_NOT_FOUND;
    int saw_busy = 0;
    vipc_status status;

    vipc_error_clear(error);
    if (!out_channel || !timeout_is_valid(timeout_ms)) {
        return set_error(
            error,
            VIPC_ERR_INVALID_ARGUMENT,
            VIPC_PHASE_CONNECT,
            ERROR_INVALID_PARAMETER);
    }
    *out_channel = NULL;

    status = build_pipe_name(endpoint, pipe_name, error);
    if (status != VIPC_OK) return status;

    deadline = deadline_from_timeout(timeout_ms);

    for (;;) {
        DWORD remaining;
        pipe = CreateFileW(
            pipe_name,
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED
                | SECURITY_SQOS_PRESENT
                | SECURITY_IDENTIFICATION,
            NULL);
        if (pipe != INVALID_HANDLE_VALUE) break;

        winerr = GetLastError();
        if (winerr == ERROR_ACCESS_DENIED) {
            return set_error(
                error,
                VIPC_ERR_ACCESS_DENIED,
                VIPC_PHASE_CONNECT,
                winerr);
        }
        if (winerr != ERROR_FILE_NOT_FOUND
            && winerr != ERROR_PIPE_BUSY
            && winerr != ERROR_SEM_TIMEOUT) {
            return set_error(
                error,
                VIPC_ERR_IO,
                VIPC_PHASE_CONNECT,
                winerr);
        }

        if (winerr == ERROR_PIPE_BUSY || winerr == ERROR_SEM_TIMEOUT) {
            saw_busy = 1;
        }

        remaining = remaining_timeout(deadline);
        if (remaining == 0u) {
            return set_error(
                error,
                saw_busy ? VIPC_ERR_ENDPOINT_BUSY : VIPC_ERR_ENDPOINT_NOT_FOUND,
                VIPC_PHASE_CONNECT,
                winerr);
        }

        if (saw_busy) {
            DWORD wait_ms = min_dword(remaining, VIPC_CONNECT_POLL_MS);
            (void)WaitNamedPipeW(pipe_name, wait_ms);
        } else {
            Sleep(min_dword(remaining, VIPC_CONNECT_SLEEP_MS));
        }
    }

    status = channel_from_pipe(pipe, 0, out_channel, error);
    if (status != VIPC_OK) return status;
    return VIPC_OK;
}

void vipc_channel_destroy(vipc_channel *channel) {
    HANDLE heap;
    LONG read_busy;
    LONG write_busy;

    if (!channel) return;
    heap = GetProcessHeap();
    (void)InterlockedExchange(&channel->poisoned, 1);
    close_channel_pipe(channel);

    read_busy = InterlockedCompareExchange(&channel->read_busy, 0, 0);
    write_busy = InterlockedCompareExchange(&channel->write_busy, 0, 0);
    if (read_busy != 0 || write_busy != 0 || channel_has_unsettled_io(channel)) {
        /*
         * Concurrent destroy is a caller lifecycle bug, but leaking a tiny
         * channel object is safer than freeing OVERLAPPED storage the kernel or
         * another thread can still touch.
         */
        return;
    }

    if (channel->read_event) (void)CloseHandle(channel->read_event);
    if (channel->write_event) (void)CloseHandle(channel->write_event);
    (void)HeapFree(heap, 0, channel);
}

int vipc_channel_is_open(const vipc_channel *channel) {
    HANDLE pipe;
    if (!channel) return 0;
    pipe = (HANDLE)InterlockedCompareExchangePointer(
        (PVOID volatile *)&((vipc_channel *)channel)->pipe,
        NULL,
        NULL);
    return pipe != NULL
        && pipe != INVALID_HANDLE_VALUE
        && InterlockedCompareExchange(
            (volatile LONG *)&((vipc_channel *)channel)->poisoned,
            0,
            0) == 0;
}

uint32_t vipc_channel_peer_pid(const vipc_channel *channel) {
    return channel ? channel->peer_pid : 0u;
}

uint32_t vipc_channel_peer_session_id(const vipc_channel *channel) {
    return channel ? channel->peer_session_id : 0u;
}

vipc_status vipc_channel_send(
    vipc_channel *channel,
    const vipc_message *message,
    const void *payload,
    uint32_t timeout_ms,
    vipc_error *error) {
    uint8_t header[VIPC_WIRE_HEADER_SIZE];
    vipc_protocol_status protocol_status;
    vipc_status status;
    ULONGLONG deadline;

    vipc_error_clear(error);
    if (!channel
        || !message
        || !timeout_is_valid(timeout_ms)
        || (message->payload_size != 0u && !payload)) {
        return set_error(
            error,
            VIPC_ERR_INVALID_ARGUMENT,
            VIPC_PHASE_WRITE_HEADER,
            ERROR_INVALID_PARAMETER);
    }
    if (!vipc_channel_is_open(channel)) {
        return set_error(
            error,
            VIPC_ERR_NOT_CONNECTED,
            VIPC_PHASE_WRITE_HEADER,
            ERROR_INVALID_HANDLE);
    }

    protocol_status = vipc_protocol_encode(message, header);
    status = map_protocol_status(
        protocol_status,
        VIPC_PHASE_WRITE_HEADER,
        error);
    if (status != VIPC_OK) return status;

    if (InterlockedCompareExchange(&channel->write_busy, 1, 0) != 0) {
        return set_error(
            error,
            VIPC_ERR_BUSY,
            VIPC_PHASE_WRITE_HEADER,
            ERROR_BUSY);
    }

    deadline = deadline_from_timeout(timeout_ms);
    status = io_exact(
        channel,
        0,
        header,
        VIPC_WIRE_HEADER_SIZE,
        deadline,
        VIPC_PHASE_WRITE_HEADER,
        error);
    if (status == VIPC_OK && message->payload_size != 0u) {
        status = io_exact(
            channel,
            0,
            (void *)payload,
            message->payload_size,
            deadline,
            VIPC_PHASE_WRITE_PAYLOAD,
            error);
    }

    if (status != VIPC_OK) poison_channel(channel);
    (void)InterlockedExchange(&channel->write_busy, 0);
    return status;
}

vipc_status vipc_channel_receive(
    vipc_channel *channel,
    vipc_message *out_message,
    void *payload_buffer,
    uint32_t payload_capacity,
    uint32_t *out_payload_size,
    uint32_t timeout_ms,
    vipc_error *error) {
    uint8_t header[VIPC_WIRE_HEADER_SIZE];
    uint8_t drain_buffer[VIPC_DRAIN_CHUNK];
    vipc_protocol_status protocol_status;
    vipc_status status;
    ULONGLONG deadline;
    uint32_t remaining;

    vipc_error_clear(error);
    if (!channel
        || !out_message
        || !out_payload_size
        || !timeout_is_valid(timeout_ms)
        || (payload_capacity != 0u && !payload_buffer)) {
        return set_error(
            error,
            VIPC_ERR_INVALID_ARGUMENT,
            VIPC_PHASE_READ_HEADER,
            ERROR_INVALID_PARAMETER);
    }
    *out_payload_size = 0u;

    if (!vipc_channel_is_open(channel)) {
        return set_error(
            error,
            VIPC_ERR_NOT_CONNECTED,
            VIPC_PHASE_READ_HEADER,
            ERROR_INVALID_HANDLE);
    }

    if (InterlockedCompareExchange(&channel->read_busy, 1, 0) != 0) {
        return set_error(
            error,
            VIPC_ERR_BUSY,
            VIPC_PHASE_READ_HEADER,
            ERROR_BUSY);
    }

    deadline = deadline_from_timeout(timeout_ms);
    status = io_exact(
        channel,
        1,
        header,
        VIPC_WIRE_HEADER_SIZE,
        deadline,
        VIPC_PHASE_READ_HEADER,
        error);
    if (status != VIPC_OK) {
        poison_channel(channel);
        (void)InterlockedExchange(&channel->read_busy, 0);
        return status;
    }

    protocol_status = vipc_protocol_decode(header, out_message);
    status = map_protocol_status(
        protocol_status,
        VIPC_PHASE_READ_HEADER,
        error);
    if (status != VIPC_OK) {
        poison_channel(channel);
        (void)InterlockedExchange(&channel->read_busy, 0);
        return status;
    }

    if (out_message->payload_size == 0u) {
        (void)InterlockedExchange(&channel->read_busy, 0);
        return VIPC_OK;
    }

    if (out_message->payload_size <= payload_capacity) {
        status = io_exact(
            channel,
            1,
            payload_buffer,
            out_message->payload_size,
            deadline,
            VIPC_PHASE_READ_PAYLOAD,
            error);
        if (status == VIPC_OK) {
            *out_payload_size = out_message->payload_size;
        } else {
            poison_channel(channel);
        }
        (void)InterlockedExchange(&channel->read_busy, 0);
        return status;
    }

    remaining = out_message->payload_size;
    while (remaining != 0u) {
        uint32_t chunk = remaining > VIPC_DRAIN_CHUNK
            ? VIPC_DRAIN_CHUNK
            : remaining;
        status = io_exact(
            channel,
            1,
            drain_buffer,
            chunk,
            deadline,
            VIPC_PHASE_READ_PAYLOAD,
            error);
        if (status != VIPC_OK) {
            poison_channel(channel);
            (void)InterlockedExchange(&channel->read_busy, 0);
            return status;
        }
        remaining -= chunk;
    }

    (void)InterlockedExchange(&channel->read_busy, 0);
    return set_error(
        error,
        VIPC_ERR_BUFFER_TOO_SMALL,
        VIPC_PHASE_READ_PAYLOAD,
        ERROR_INSUFFICIENT_BUFFER);
}