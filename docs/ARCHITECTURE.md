# Architecture

## 1. Ownership boundary

VectorIPC owns a **local process boundary**. It does not own the product protocol
or the helper's business logic.

```text
                              ┌───────────────────────────────┐
ExtendScript                  │ ExternalObject adapter       │
  synchronous host call ─────►│ tiny ABI / bounded façade    │
                              └───────────────┬───────────────┘
                                              │
Illustrator native plug-in                    │
  worker thread ──────────────────────────────┤
                                              ▼
                                  ┌──────────────────────┐
                                  │ VectorIPC channel     │
                                  │ framing + deadlines  │
                                  └──────────┬───────────┘
                                             │
                         ┌───────────────────┴──────────────────┐
                         ▼                                      ▼
                  Windows named pipe                  Unix-domain socket
                    (implemented)                         (planned)
                         │
                         ▼
                out-of-process helper
                         │
              product-specific dispatcher
```

The same C API is linked directly into a native `.aip` plug-in and statically
linked into a small ExternalObject bridge DLL. There is no "ExtendScript
protocol" and no "Illustrator SDK protocol".

## 2. Layering

### Protocol core

`src/core/`

- validates message metadata;
- encodes/decodes the fixed 32-byte envelope;
- defines stable transport error/status values;
- has no operating-system or Adobe dependency.

### Platform transport

`src/platform/`

- maps an endpoint token to a local OS endpoint;
- creates/accepts/connects persistent full-duplex channels;
- performs exact framed reads/writes with one operation deadline;
- owns OS security and peer process/session metadata.

Windows v1 uses byte-mode named pipes rather than relying on named-pipe message
boundaries. The wire therefore has identical stream semantics to a future Unix
domain socket transport.

### Host adapters

`src/adapters/`

Adapters translate host constraints only.

**ExternalObject:** synchronous, flat, string/numeric Adobe ABI. It may encode an
opaque binary frame into a text-safe representation at that boundary, but the
wire itself remains binary.

**Native plug-in:** links the C API directly. A plug-in worker thread owns IPC;
the Illustrator SDK/main thread owns SDK calls. Handles/art references are never
dereferenced on the transport thread.

### Product protocol

Applications and adapters define:

- operation IDs;
- payload schemas;
- application handshake/version compatibility;
- helper lifecycle policy;
- event semantics and queues;
- product error codes.

None of those definitions belong in VectorIPC.

## 3. Concurrency model

A channel is full duplex:

- one reader may be in flight;
- one writer may be in flight concurrently;
- a second reader or second writer returns `VIPC_ERR_BUSY`.

This is enough for a native plug-in to keep a dedicated receive loop for helper
events while another thread/queue emits notifications and requests. Higher-level
request multiplexing uses the 64-bit correlation ID.

The ExternalObject adapter intentionally remains single-flight because
ExtendScript calls are synchronous and Illustrator host failures can bypass
JavaScript `try/catch`.

## 4. Deadline model

Each public transport call receives one timeout for the **whole operation**, not
one timeout per syscall.

```text
deadline = monotonic_now + timeout

write header ─┐
write payload ├─ all consume the same remaining budget
              │
read header  ─┤
read payload ─┘
```

Windows I/O uses overlapped operations. On deadline expiry VectorIPC asks the
kernel to cancel the specific I/O, then permits only a small bounded settle
window needed to keep the `OVERLAPPED` storage valid. A channel whose
cancellation cannot settle is poisoned and never reused.

Accepted server pipe instances are never reused. Graceful destruction therefore
closes the server handle without `DisconnectNamedPipe`, preserving already
buffered final bytes for the client to drain. Poison/abort paths use a forced
disconnect, where discarding unread bytes is intentional.

## 5. Endpoint model

Applications provide a conservative endpoint token, for example:

```text
sample-app
arcfit
com.example.tool
```

Allowed token bytes:

```text
[A-Za-z0-9._-]
```

Windows maps the token to:

```text
\\.\pipe\VectorIPC.<windows-session-id>.<token>
```

The path is local-only and the server DACL contains only the current user.
Session scoping prevents ordinary cross-session collisions. The product must
still treat every message as untrusted input.

Windows clients also request `SecurityIdentification` explicitly when opening
the pipe. A helper can identify its peer but cannot use the pipe to impersonate
the Illustrator client's security token.

## 6. Worker lifecycle is intentionally separate

VectorIPC does not automatically spawn, replace, update, or kill helpers. Those
policies are application concerns and differ materially between products.

A later optional `runtime/` layer may standardize:

- spawn;
- ownership leases;
- build-pair handshakes;
- health probes;
- stale-helper replacement;
- graceful quit;
- crash/restart accounting.

That layer must be built on top of the transport rather than fused into it.

## 7. Why ArcFit is precedent, not source code

ArcFit proved the process-isolation model and hard-timeout need in real
Illustrator use. VectorIPC intentionally changes several ArcFit-specific choices:

| ArcFit bridge | VectorIPC |
|---|---|
| line-oriented `key=value` | fixed binary envelope + opaque payload |
| product operation strings | product-defined `uint32` operation IDs |
| one request per connection | persistent full-duplex channel |
| single-flight only | one reader + one writer, correlation IDs |
| Windows product pipe name | validated, session-scoped endpoint token |
| default pipe DACL | explicit current-user DACL |
| message-mode dependency | portable byte-stream framing |

ArcFit and other consumers can forward-port after VectorIPC itself has a stable,
measured contract.