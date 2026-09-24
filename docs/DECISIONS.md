# Architecture Decisions

This file records decisions that are expensive to rediscover and easy to
accidentally undo.

## D1 — VectorIPC is below ExtendScript and the Illustrator SDK

**Decision:** keep the project product-neutral and host-neutral. ExtendScript is
an adapter; a native Illustrator plug-in links the same C ABI directly.

**Why:** naming and designing the core around ExternalObject would force
synchronous/string constraints onto native plug-ins.

## D2 — Fixed binary envelope, opaque payload

**Decision:** wire v1 uses a fixed 32-byte little-endian header. Payload bytes are
opaque to VectorIPC.

**Why:** product serialization evolves independently. The transport must not
require ESON, CBOR, protobuf, or any product schema.

## D3 — Byte-stream semantics on every platform

**Decision:** Windows named pipes use byte mode even though Windows also offers
message-mode pipes.

**Why:** framing stays identical to Unix-domain sockets on macOS, exact-length
reads remain explicit, and the protocol is not coupled to one OS transport's
message-boundary feature.

## D4 — Persistent full-duplex channels

**Decision:** a channel supports one concurrent reader and one concurrent writer.
Correlation IDs belong in the base envelope.

**Why:** native plug-ins need to receive helper events while emitting requests or
notifications. ArcFit-style connect/request/read/close is too restrictive for a
generic substrate.

## D5 — One operation deadline

**Decision:** each send/receive/accept/connect call consumes one monotonic timeout
budget across all underlying waits and partial I/O.

**Why:** per-syscall timeouts can multiply into an unexpectedly long host stall.

## D6 — Explicit Windows security

**Decision:** Windows pipe servers use a current-user-only DACL,
`PIPE_REJECT_REMOTE_CLIENTS`, a Windows-session-scoped endpoint name, and clients
request `SecurityIdentification`.

**Why:** local IPC should not inherit permissive default DACL behavior or give a
helper unnecessary ability to impersonate the Adobe host token.

## D7 — Graceful close is not DisconnectNamedPipe

**Decision:** accepted server instances are never reused. Normal channel
destruction uses `CloseHandle` without `DisconnectNamedPipe`. Poison/abort
paths force-disconnect.

**Why:** Windows discards unread data on `DisconnectNamedPipe`. A normal close
must allow the peer to drain a final response/event already accepted by the
kernel.

This distinction is covered by the last-frame stress and full-duplex tests.

## D8 — Helper lifecycle stays above transport

**Decision:** v1 transport does not spawn, replace, update, or kill a product
helper.

**Why:** ownership policy differs between products. A later reusable runtime
layer can standardize leases, health, replacement, and graceful shutdown without
polluting the byte transport.

## D9 — No unverified macOS implementation

**Decision:** non-Windows currently returns `VIPC_ERR_UNSUPPORTED`.

**Why:** the intended Unix-domain-socket implementation must be exercised on a
real macOS Adobe target before the project claims cross-platform transport
support.

## D10 — One ExternalObject binding

**Decision:** the ExtendScript adapter exports one script-visible method,
`vipc(...)`, and multiplexes its small command set through the first numeric
argument.

**Why:** live Illustrator research found per-method binding failures that were
stable for a given DLL build. One first/only critical binding minimizes that
failure surface and makes the adapter contract easier to probe before use.

## D11 — Arbitrary binary uses numeric packed ingress, Base64 string egress

**Decision:** ExternalObject **arbitrary-binary** ingress packs six bytes into
each exact 48-bit JavaScript Number. Received arbitrary-binary payloads are
returned as Base64 inside an ASCII response envelope. The adapter cap is
256 KiB. Adapter v3 additionally has a separate validated NUL-free UTF-8 text
transaction whose request/response uses the host string lane directly.

**Why:** numeric arguments are the empirically reliable lane for arbitrary
bytes. `kTypeString` cannot represent arbitrary raw binary because NUL and
encoding boundaries matter. Base64 is ASCII-safe, while a separately validated
UTF-8/no-NUL text contract can use strings without pretending they are a binary
transport. The 256 KiB payload cap keeps Base64 expansion within the already
measured safe return envelope.

This encoding exists only at the ExtendScript ABI boundary. The VectorIPC wire
continues to carry opaque raw bytes.

## D12 — Composite ExternalObject transactions use one deadline budget

**Decision:** `TRANSACT` starts one wall-clock timeout budget before send and
passes only the remaining milliseconds to receive.

**Why:** host safety is defined by the operation the script invoked. A nominal
2-second transaction must not silently become roughly 4 seconds because its
send and receive legs each received a fresh 2-second allowance.

## D13 — The ES3 wrapper owns composition ergonomics, not serialization

**Decision:** `VectorIPC.jsx` handles host-boundary mechanics only: adapter
loading, endpoint packing, staging, correlation pairs, response parsing, and a
correctness-path Base64 decoder. It does not embed or require
JSON/ESON/CBOR/product schemas. It may expose thin optional composition helpers
for an already-loaded caller-supplied codec.

**Why:** serializer policy belongs to consumers, but forcing every product to
rewrite the same one-line composition creates avoidable mistakes. Wrapper v5
therefore supports `requestESON/requestJSON`, ESB64-backed binary-string
decode, and ESCHARS Base64-to-hex when those libraries are supplied or already
loaded. VectorIPC core and the native SDK surface remain dependency-free and
payload-opaque.

## D14 — ExternalObject v2 introduced generation-tagged logical sessions

**Decision:** one loaded DLL can own up to 16 independent session records. Every
successful CONNECT returns a numeric handle containing a slot token plus a
generation. All stage/transact/send/receive/close commands require that handle.

**Why:** ArcFit-style global connection state would make unrelated scripts or
products overwrite each other's staged payload/channel. Generation validation
also prevents an ordinary stale numeric handle from silently targeting a later
session that reused the same slot.

The session registry is state isolation, not parallel host execution. The single
`vipc(...)` ExternalObject entry remains globally serialized.

Adapter v3 retains this session model unchanged while adding the validated text
transaction.

## D15 — One canonical ExtendScript wrapper

**Decision:** `src/adapters/externalobject/VectorIPC.jsx` is the only maintained
ES3 wrapper implementation. It exports `VectorIPCExternalObject` and the shorter
`VectorIPC` alias to the same object. `VectorIPC.jsxinc` is a byte-identical
distribution mirror, not a second implementation; the build gate rejects any
divergence.

**Why:** two wrappers tracking different adapter revisions created an avoidable
split-brain risk during the v1→v2 transition. One implementation keeps command
arity, session handles, capability negotiation, staging behavior, and helpers
version-locked to the native adapter.

## D16 — Native C++ is an ownership façade, not another runtime

**Decision:** `vipc.hpp` provides only move-only, non-throwing RAII ownership
for `vipc_channel*` and `vipc_server*`, plus direct forwarding of the C
operations.

**Why:** native Illustrator plug-ins benefit from deterministic ownership, but
VectorIPC should not decide their thread model, queueing policy, serializer,
exception policy, or SDK marshaling strategy. Keeping the façade zero-policy
also makes the C ABI remain the architectural source of truth.

## D17 — Package consumption is a tested contract

**Decision:** VectorIPC exports `VectorIPC::vectoripc` through an installed CMake
config and installs C/C++ headers plus optional ExternalObject runtime assets.
The normal repository test gate validates a fresh out-of-tree
`find_package(VectorIPC CONFIG REQUIRED)` consumer.

**Why:** copying source files or hard-coding build-tree paths would undermine the
goal of a reusable foundation. Packaging regressions are API regressions and
belong in the normal gate.

## D18 — Binary strings/text are the ExtendScript bulk-performance lanes

**Decision:** prefer:

1. `requestText/requestESON` for NUL-free UTF-8 structured/control data;
2. `requestBinaryString` when arbitrary bytes already live in a JavaScript
   byte string;
3. ESB64-backed `decodeBase64BinaryString` when a response should remain a
   byte string;
4. ESCHARS-backed `decodeBase64Hex` when the consumer wants hexadecimal text.

`requestByteArray` and numeric-array `decodeBase64` remain exact compatibility
lanes, not the recommended bulk representation.

**Why:** live Illustrator profiling found the named-pipe/ExternalObject boundary
is tiny compared with ES3 numeric-array traversal. At 64 KiB, numeric-array
packing took roughly 4 seconds while binary-string packing took ~0.11 seconds.
For a 65,535-byte Base64 response, numeric-array decode took ~1.9–2.3 seconds;
ESB64 native binary-string decode took ~1.08–1.16 milliseconds for NUL-free
output, while ESB64's exact NUL-safe ES3 fallback remained ~82–88 milliseconds.
A hardened direct text round trip measured ~1.15 ms at 64 KiB and ~4.44 ms at
256 KiB (median of run medians). Those final values include surrogate-pair
validation required to prevent a live-observed Illustrator boundary corruption
where a lone surrogate became an empty string.

The transport must not hide this engine behavior behind an apparently equivalent
API. Representation choice is part of host performance.

## D19 — Live ExternalObject copies live in user temp, never the build tree

**Decision:** live probes and live benchmarks stage uniquely named DLL copies
under the current user's temporary `vector-ipc-live` directory. They use a
content-only copy and best-effort stale pruning; the canonical build DLL is never
loaded directly by a probe.

**Why:** Illustrator 30.6.0 can keep an ExternalObject module mapped after
`unload()`. When the repository itself lives under `Program Files`, build-tree
copies also inherit ACLs that make those pinned artifacts difficult to clean.
User-temp staging keeps host module caching from polluting or permission-locking
the repository while preserving per-build unique-path probing.

## D20 — ABI v1 uses fixed-width scalar types and locked structure layouts

**Decision:** public statuses, phases, protocol statuses, and message kinds are
`uint32_t` typedefs rather than C enum-typed ABI fields. `vipc_error` is fixed at
12 bytes and `vipc_message` at 24 bytes with compile-time asserted offsets. The
32-byte wire envelope remains separately encoded/decoded and unchanged.

**Why:** C enum representation and implicit structure padding are implementation
choices. A reusable native foundation should fail at compile time rather than
silently expose a compiler-dependent binary layout. This is a pre-release ABI
decision, so v1 starts with the deterministic layout instead of carrying a
legacy alias/layout forward.

## D21 — Destruction is not asynchronous cancellation

**Decision:** callers must not race `vipc_server_destroy()` with `accept()` or
`vipc_channel_destroy()` with `send()` / `receive()`. Shutdown uses the existing
finite operation deadlines: signal product shutdown, let the call return, join
the owning worker, then destroy the object.

**Why:** Windows overlapped I/O requires its `OVERLAPPED` storage to remain valid
until completion/cancellation settles. The implementation fails safe by
quarantining storage rather than risking use-after-free in misuse/catastrophic
paths, but intentional concurrent destruction would turn that safety valve into
a lifecycle leak. If products later need active cross-thread interruption, it
should be a dedicated, explicitly tested cancellation API.

## D22 — Listener instances recover from transient pre-accept disconnects

**Decision:** every failed Windows accept attempt that leaves the current named-
pipe instance uncertain or unusable replaces that listener instance before
returning. If listener recreation itself fails, the recovery failure takes
precedence and the server is poisoned. A client that connects and disappears
before `vipc_server_accept()` may surface as `VIPC_ERR_PEER_CLOSED`, a bounded
timeout, or a race-success channel, but it must not permanently disable the
server.

**Why:** Windows permits a client to connect between `CreateNamedPipeW()` and
`ConnectNamedPipe()`, and the client can disappear again before the server binds
or consumes the accept operation. Returning that transient without replacing the
pipe can strand subsequent clients behind a dead listener. The normal self-test
repeats this race 32 times, immediately accepts a healthy client on the same
server object after every transient, and verifies that process handle count
remains stable.