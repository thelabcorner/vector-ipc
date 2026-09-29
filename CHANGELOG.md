# Changelog

All notable VectorIPC changes are recorded here.

VectorIPC uses semantic product versions. The product version is intentionally
independent of the C ABI version, wire protocol version, ExternalObject adapter
version, and ExtendScript wrapper version.

## [0.1.3] - 2026-09-29

### Added

- Added `vipc_channel_wait_readable()` and the C++ `Channel::wait_readable()`
  façade for bounded, non-consuming readiness waits on persistent channels.
  This API is new in v0.1.3 and ships with its complete failure contract:
  `VIPC_ERR_TIMEOUT` consumes no bytes and leaves the channel open and
  synchronized, `VIPC_ERR_BUSY` returns immediately, and a definitive terminal
  failure — `VIPC_ERR_PEER_CLOSED` or a real I/O failure — closes the channel
  before returning, so `vipc_channel_is_open()` reports the truth and later
  operations report `VIPC_ERR_NOT_CONNECTED`.
- Added `vectoripc_readiness_stress` covering repeated idle readiness timeouts,
  peer-close detection, single-reader-slot `VIPC_ERR_BUSY` collisions, and
  long-idle duplex survival. It runs in the Release, `/analyze`, and AddressSanitizer
  gates, and a longer configuration runs in `npm run test:stress`.
- Added `vectoripc_readiness_idle_bench` (`npm run bench:idle`) which measures
  idle CPU with `GetThreadTimes`/`GetProcessTimes` against wall time, with a
  counter liveness check, tick-quantum detection, a matched blocked control, and
  sample count/median/p95 reporting.

### Changed

- A `vipc_channel_receive()` timeout retains its existing hard-deadline poison
  semantics because a partial framed read may already have consumed bytes. A
  `vipc_channel_wait_readable()` timeout is non-destructive by contrast: it
  returns `VIPC_ERR_TIMEOUT` without consuming bytes or poisoning the channel.
- The implementation's readiness poll constant (`VIPC_READABLE_POLL_MS`, 2 ms)
  is documented as a nominal poll period rather than a latency promise:
  `Sleep()` rounds up to the 15.625 ms system timer tick, so readiness is
  tick-bounded. See decision D23 and `docs/BENCHMARKS.md`.

### Compatibility

- Product version advances to `0.1.3`.
- C ABI version remains `1`; the new entry point is additive and existing
  structs/layouts are unchanged.
- Wire protocol remains `VIPC/1.0`.

### Measured

- Idle readiness over 300 s of continuous waiting: **0.104 % of one core**
  (conservative bound 0.130 %), against a matched 300 s blocked control of
  0.000 ms process CPU.
- Readiness wake latency: **8.116 ms median / 14.972 ms p95**, bounded by the
  15.625 ms system timer tick that quantizes the nominal 2 ms poll. `Sleep()`
  rounds up to the tick, so `VIPC_READABLE_POLL_MS` is a nominal period and not
  a latency promise. See `docs/BENCHMARKS.md` and decision D23.

## [0.1.2] - 2026-09-23

Patch release: consolidate the ExternalObject adapter's return-value helpers on
ESABI's canonical value setters.

### Changed

- Replaced the adapter's remaining hand-written `esabi_value` field assignments
  with `esabi_value_set_undefined()` and `esabi_value_set_string()` from
  [ESABI v0.3.0](https://github.com/thelabcorner/esabi/releases/tag/v0.3.0).
- No behavioral change: the ESABI setters perform the same reset, type-tag, and
  reserved-field writes, and the adapter's ownership, error, and payload
  semantics are unchanged.

### Compatibility

- Product version: `0.1.2`.
- C ABI remains `1`.
- Wire protocol remains `VIPC/1.0`.
- ExternalObject adapter remains `3`.
- ExtendScript wrapper remains `5`.
- ESABI dependency remains `v0.3.0` / `3e99040c43cef573b477ad372b2a3a96c4d3a7d7`.

## [0.1.1] - 2026-09-23

Patch release: migrate the ExternalObject host ABI definitions to ESABI v0.3.0.

### Changed

- Replaced VectorIPC's private copy of the ExtendScript ExternalObject
  `TaggedData` layout, type tags, calling convention, lifecycle exports, and
  packing declarations with the canonical `esabi::esabi` interface from
  [ESABI v0.3.0](https://github.com/thelabcorner/esabi/releases/tag/v0.3.0).
- ExternalObject builds now accept exactly ESABI `0.3.0` when installed and
  otherwise fetch the peeled v0.3.0 commit
  `3e99040c43cef573b477ad372b2a3a96c4d3a7d7` as an `EXCLUDE_FROM_ALL` private
  dependency, so VectorIPC does not repackage ESABI's headers/CMake files.
- Corrected the native `ESInitialize` declaration from a historical
  pointer-to-pointer form to ESABI's documented value-array pointer signature.
  VectorIPC never dereferenced that argument, so runtime behavior is unchanged.
- Updated native adapter smoke/session tests to exercise the same ESABI types
  and constants used by production code.

### Compatibility

- Product version: `0.1.1`.
- C ABI remains `1`.
- Wire protocol remains `VIPC/1.0`.
- ExternalObject adapter remains `3`.
- ExtendScript wrapper remains `5`.
- No application operation IDs, wire bytes, payload limits, or wrapper commands
  changed in this release.

## [0.1.0] - 2026-09-23

First pre-release foundation.

### Added

- Product-neutral VectorIPC identity and `VIPC/1.0` wire protocol.
- Stable C11 API with fixed-width ABI v1 types and compile-time layout checks.
- Header-only C++17 ownership façade under `vectoripc`.
- Persistent Windows named-pipe server/client transport.
- Same-user, same-session, local-only Windows endpoint security.
- Full-duplex channels with independent send/receive serialization.
- Hard operation deadlines, bounded cancellation cleanup, and poisoned-channel
  semantics for ambiguous partial I/O.
- Recoverable Windows listener state after transient pre-accept disconnects.
- 16 MiB wire payload limit and explicit buffer-too-small drain semantics.
- ExternalObject adapter v3 with 16 generation-tagged logical sessions.
- ExtendScript wrapper v5, including text, ESON, ESB64, and ESCHARS composition
  helpers without making those libraries core dependencies.
- User-temp staging for disposable live Illustrator DLL probes.
- CMake install/export package as `VectorIPC::vectoripc`.
- Independent installed-package C11 and C++17 consumers.
- Deterministic protocol fuzzing, static analysis, AddressSanitizer, stress,
  soak, boundary, cancellation, and live Illustrator certification gates.
- Reproducible release manifest tooling with source/public-header SHA-256
  digests and a tagged source archive.

### Validated

- Windows x64 native core.
- Live Adobe Illustrator 30.6.0 ExternalObject integration.
- 500,000 deterministic protocol fuzz cases.
- 16 MiB exact wire boundary.
- 100,000 simultaneous messages in each full-duplex direction.
- 1,000,000-frame small-payload soak.
- Repeated cancellation/resource tests with stable process handle counts.

### Known limitations

- The v0.1.0 transport implementation is Windows named pipes only.
  macOS/POSIX currently builds an explicit unsupported stub; Unix-domain
  sockets are planned.
- Destruction is not a cross-thread cancellation API. Callers should use finite
  operation deadlines, join the owning worker, and then destroy the object.
- Numeric-array bulk staging in legacy ExtendScript is intrinsically expensive;
  text/binary-string representations are the preferred scripting bulk paths.
- The public v0.1.0 release is licensed under the MIT License.
