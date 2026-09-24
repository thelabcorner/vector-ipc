# Host Adapters

## ExternalObject

ExternalObject is not the VectorIPC core ABI. It is a synchronous adapter over it.

The repository skills establish several constraints that the adapter must honor:

- canonical Adobe direct-interface `TaggedData` ABI, 8-byte packed;
- small, explicit `ESInitialize` method surface;
- exact returned-string ownership through `ESFreeMem`;
- no negative fatal `kESErr*` method results;
- no raw pointers/object graphs crossing into JSX;
- raw `kTypeString` is not an arbitrary-byte channel: embedded NUL truncates,
  and the UTF-16 surrogate window is unsafe for packed byte values;
- some host failures can bypass JSX `try/catch`;
- loaded DLLs can remain locked/cached until Illustrator exits.

The implemented adapter exposes a **text-safe façade** over VectorIPC frames rather
than pretending `TaggedData` can carry the native binary API directly.

It intentionally registers **one method only**, `vipc(...)`. A numeric command
ID selects connect/stage/transact/send/receive/close behavior. This keeps the
critical surface as small as possible in light of the measured per-method
ExternalObject binding instability.

Arbitrary-binary ingress never depends on string arguments. Arbitrary bytes are
packed six at a time into exact 48-bit IEEE-754 integer values and passed through
the reliable numeric argument lane. Binary egress uses ASCII `kTypeString`
values with Base64 payloads.

Adapter v3 separately exposes `TRANSACT_TEXT` for NUL-free UTF-8 text. That
lane validates UTF-8 natively and returns a text response only when the helper
payload is valid UTF-8 without NUL. It is intentionally not an arbitrary-binary
escape hatch.

Adapter failures are encoded in the returned ASCII value and methods return
`kESErrOK`, so VectorIPC never intentionally emits a fatal negative
ExtendScript runtime error.

The adapter has a **256 KiB payload cap**, independent of the core wire's 16 MiB
hard cap. A 256 KiB payload expands to ~341 KiB of Base64, keeping the return
lane within the ~360 KiB string size already exercised by the repository's
ExternalObject research. Larger product artifacts should use a native plug-in,
file/CAS handoff, or another product-owned bulk channel rather than forcing
megabytes through ExtendScript.

Adapter v3 maintains up to **16 generation-tagged logical sessions**. Each
session owns its channel, staged payload, receive buffer, and generation-aware
handle. This isolates independent consumers without pretending ExternalObject
itself is a parallel execution environment: the sole `vipc(...)` host entry
point remains globally serialized.

The ES3 wrapper in `src/adapters/externalobject/VectorIPC.jsx` additionally
handles variable-arity `Function.apply` calls, a default and hard maximum
**1,536-byte staging chunk** (256 packed doubles), 64-bit correlation IDs as two
uint32 values, adapter-v3 compatibility negotiation, convenience request methods,
and Base64 response decoding. The chunk size is overridable downward; the live
certification forces 768-byte chunks to exercise multi-call staging. Larger host
calls are rejected before adapter state is touched.

Wrapper v5 also exposes optional composition helpers without adding runtime
dependencies:

- `requestESON/requestJSON` → supplied/already-loaded ESON-compatible codec +
  the direct text lane;
- `decodeBase64BinaryString` → supplied/already-loaded ESB64-compatible codec;
- `decodeBase64Hex` → supplied/already-loaded ESCHARS-compatible codec.

For bulk ExtendScript workloads, text/binary-string representations are preferred
over numeric byte arrays. Live profiling found 64 KiB numeric-array staging at
roughly 4 seconds versus ~0.11 seconds for binary-string packing, while a direct
64 KiB hardened UTF-8 request/response measured ~1.15 ms median-of-run-medians.
Numeric-array APIs remain
exact compatibility lanes.

See `EXTERNALOBJECT.md` for the exact command/response grammar and live-host
evidence.

## Native Illustrator plug-in

The native plug-in adapter is intentionally thin: C code calls the stable ABI
directly; C++17 consumers may use the header-only `vipc.hpp` move-only RAII
owners over that exact ABI.

Rules:

1. Illustrator SDK calls remain on the SDK/main thread.
2. IPC runs on a plug-in worker thread.
3. Events captured from notifiers are normalized/copied before they leave the SDK
   thread.
4. No `AIArtHandle` or SDK object is dereferenced from an IPC thread.
5. The product owns queueing/backpressure/coalescing policy.
6. The helper's responses/events are treated as untrusted and validated before
   any SDK mutation is scheduled.

There are no hidden VectorIPC threads. The product owns its worker/receiver
threads, queue/backpressure/coalescing policy, and application handshake. See
`NATIVE.md`.

## COM

COM is a **verification/automation plane**, not another VectorIPC transport. It is
useful for launching JSX probes and verifying the ExternalObject adapter in a
real Illustrator process. Live tests must use bounded COM calls and the shared
COM lock supplied by the repository's Illustrator automation skill.

## UXP

Illustrator UXP is currently an internal, version-sensitive Adobe surface with no
public third-party distribution contract. It is not an VectorIPC v1 target and
must not influence the core protocol.