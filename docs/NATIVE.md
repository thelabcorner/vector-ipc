# Native C / C++ Consumption

VectorIPC's native surface deliberately contains no Illustrator SDK types and no
hidden worker threads. A native `.aip`, helper executable, or other local
process links the same transport library.

## CMake

After installing VectorIPC:

```cmake
find_package(VectorIPC CONFIG REQUIRED)

target_link_libraries(my_plugin PRIVATE VectorIPC::vectoripc)
```

The package exports the C API and the header-only C++17 ownership façade:

```cpp
#include <vectoripc/vipc.h>   // C ABI
#include <vectoripc/vipc.hpp> // optional C++17 RAII
```

The C++ façade has no independent protocol, runtime, allocation layer, or
exception policy. It is ownership ergonomics over the C ABI.

## ABI v1 layout

VectorIPC's public scalar status/kind types are fixed-width `uint32_t` typedefs;
the ABI does not depend on compiler enum representation. The public structures
are compile-time locked by the installed headers:

| Type | Size | Stable offsets |
|---|---:|---|
| `vipc_error` | 12 B | `status=0`, `phase=4`, `platform_code=8` |
| `vipc_message` | 24 B | `kind=0`, `flags=4`, `operation=8`, `payload_size=12`, `correlation_id=16` |

`vipc_message` is an in-memory API descriptor, **not** the wire header. The
codec maps those fields explicitly into the independent 32-byte little-endian
wire envelope, so locking the native ABI does not change protocol v1 bytes.

## C++17 client

```cpp
#include <vectoripc/vipc.hpp>

vectoripc::Channel channel;
vectoripc::Error error;

if (vectoripc::Channel::connect(
        "my-product",
        2000,
        channel,
        &error) != VIPC_OK) {
    // Product decides how to surface/retry the failure.
}
```

`Channel` and `Server` are:

- move-only;
- deterministic RAII owners;
- non-throwing;
- thin enough to optimize away to the C calls;
- explicit about timeouts and buffers.

A failed `Channel::connect()` or `Server::open()` leaves the destination's
existing owned handle intact. A successful call replaces it.

Destruction is not a cross-thread cancellation API. `Channel::reset()` /
`vipc_channel_destroy()` must run only after in-flight readiness, send, and
receive calls have returned; `Server::reset()` / `vipc_server_destroy()` must
run only after an in-flight accept has returned. Destruction does not cancel a
peer, wait out a deadline, or interrupt a poll — it waits for nothing, so the
caller owns the join. Use finite operation/accept timeouts to observe shutdown,
join the owning worker, then destroy the object.

## Full-duplex plug-in pattern

VectorIPC permits **one reader-side operation and one write concurrently on a
channel**. "Reader-side" covers both `vipc_channel_receive()` and
`vipc_channel_wait_readable()`; they share one slot. It does not create those
threads for the consumer.

A native Illustrator plug-in should normally look like:

```text
Illustrator SDK/main thread
        │
        │ notifier data copied/normalized
        ▼
bounded product queue
        │
        ▼
IPC worker / outbound path ─────► helper
                                  │
IPC receive path ◄────────────────┘
        │
        ▼
validated product event/result
        │
        ▼
main-thread scheduling boundary
        │
        ▼
Illustrator SDK mutation
```

VectorIPC never makes an Illustrator SDK call and never dereferences
`AIArtHandle` or another SDK object. The plug-in must copy the plain data it
needs before leaving the SDK/main thread.

Whether a product uses:

- one worker thread that alternates send/receive;
- one dedicated receiver plus an outbound queue;
- an application-level request multiplexer;

is intentionally outside the transport. A long-running service and a tiny
single-purpose helper do not have the same scheduling requirements.

## Readiness wait

`vipc_channel_wait_readable()` (C) / `Channel::wait_readable()` (C++) answers one
question — is at least one byte readable? — **without consuming stream data**:

```cpp
vectoripc::Error error;

switch (channel.wait_readable(2000, &error)) {
case VIPC_OK:
    // Bytes are available; a receive can start immediately.
    break;
case VIPC_ERR_TIMEOUT:
    // Non-destructive: nothing consumed, channel still open and synchronized.
    break;
case VIPC_ERR_BUSY:
    // Another reader-side call owns the slot. Returned immediately.
    break;
default:
    // Terminal (peer closed / real I/O failure): the channel is now closed.
    break;
}
```

Use it to wait on a persistent channel that may sit idle for a long time without
starting a framed read. Like a receive, it takes a finite timeout — there is no
unbounded wait — and it occupies the channel's single reader-side slot while it
runs, so an overlapping `wait_readable()` or `vipc_channel_receive()` returns
`VIPC_ERR_BUSY` immediately rather than blocking. A send may run concurrently
with either.

### Timeout semantics differ from receive, deliberately

| Call | On timeout | Channel afterwards |
|---|---|---|
| `wait_readable()` | `VIPC_ERR_TIMEOUT` | **non-destructive** — no bytes consumed, still open and synchronized |
| `receive()` | `VIPC_ERR_TIMEOUT` | **destructive** — the hard deadline expired mid-frame, so the channel is poisoned |

A readiness timeout is safe precisely because no framed read has begun. A
receive timeout cannot make that promise: a partial header or payload may
already have been consumed and the byte stream cannot be resynchronised, so the
channel is poisoned rather than reused.

A terminal readiness failure is different again. `VIPC_ERR_PEER_CLOSED` and real
I/O failures are not timeouts: the channel is closed before the call returns, so
`vipc_channel_is_open()` reports the truth and later operations report
`VIPC_ERR_NOT_CONNECTED`. Always re-check `vipc_channel_is_open()` after a
readiness return that is neither `VIPC_OK`, `VIPC_ERR_TIMEOUT`, nor
`VIPC_ERR_BUSY`.

Readiness is delivered by a `PeekNamedPipe()` poll with a nominal 2 ms period.
`Sleep()` rounds up to the system timer tick, so observed wake latency is
tick-bounded rather than 2 ms — see [docs/BENCHMARKS.md](BENCHMARKS.md) and
decision D23. The constant is a latency knob, never a promise.

## Server/helper pattern

```cpp
vectoripc::Server server;
vectoripc::Error error;

if (server.open("my-product", &error) != VIPC_OK) {
    // Endpoint already owned, access denied, etc.
}

for (;;) {
    vectoripc::Channel client;
    if (server.accept(5000, client, &error) != VIPC_OK) {
        // Timeout can be used to service shutdown/health state.
        continue;
    }

    // Hand the accepted channel to product-owned session logic.
}
```

Accepted channels are independent of the listener. The server immediately
creates its next Windows pipe instance so a persistent client does not prevent a
second client from connecting.

## Application handshake

The transport security boundary is intentionally local:

- current-user-only Windows ACL;
- `PIPE_REJECT_REMOTE_CLIENTS`;
- Windows-session-scoped endpoint;
- client `SecurityIdentification`, not impersonation.

That prevents other users/remote machines from casually attaching, but **another
process running as the same user is still inside that OS trust boundary**.

Products that require stronger peer identity should perform an application
handshake immediately after connect. Typical inputs are:

- product protocol/build version;
- capability bits;
- a per-launch random nonce handed to the helper out of band;
- expected peer PID/build identity where the product can establish it safely.

VectorIPC deliberately does not standardize that product handshake yet. Encoding
one global policy into the transport would make a tiny ExtendScript helper and a
long-running service pay for the same lifecycle assumptions.

## Installed package

`cmake --install` installs:

- `lib/vectoripc.*`;
- `include/vectoripc/vipc.h`;
- `include/vectoripc/vipc_protocol.h`;
- `include/vectoripc/vipc.hpp`;
- `lib/cmake/VectorIPC/VectorIPCConfig*.cmake`;
- on Windows with ExternalObject enabled,
  `bin/VectorIPCExternalObject.dll`;
- `share/vectoripc/externalobject/VectorIPC.jsx[inc]`.

The repository's `npm test` gate installs to a disposable prefix, configures a
fresh external CMake consumer with `find_package(VectorIPC CONFIG REQUIRED)`,
builds it, links it, and runs it. This catches install/export regressions that an
in-tree target cannot detect.