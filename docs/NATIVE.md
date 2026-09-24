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
`vipc_channel_destroy()` must run only after in-flight send/receive calls have
returned; `Server::reset()` / `vipc_server_destroy()` must run only after an
in-flight accept has returned. Use finite operation/accept timeouts to observe
shutdown, join the owning worker, then destroy the object.

## Full-duplex plug-in pattern

VectorIPC permits **one read and one write concurrently on a channel**. It does
not create those threads for the consumer.

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