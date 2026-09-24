# Changelog

All notable VectorIPC changes are recorded here.

VectorIPC uses semantic product versions. The product version is intentionally
independent of the C ABI version, wire protocol version, ExternalObject adapter
version, and ExtendScript wrapper version.

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
