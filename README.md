<div align="center">

# VectorIPC: Bounded local IPC for scripting hosts and native plug-ins

### C11 transport core, C++17 ownership façade, and a live-certified Adobe ExtendScript ExternalObject bridge

[![Protocol](https://img.shields.io/badge/wire-VIPC%2F1.0-success)](#api)
[![Fuzz](https://img.shields.io/badge/protocol-500k%20cases-purple)](#validation)
[![Illustrator](https://img.shields.io/badge/Illustrator-30.6.0%20live--certified-success)](#compatibility)
[![Native](https://img.shields.io/badge/native-Windows%20x64-blue)](#compatibility)
[![ABI](https://img.shields.io/badge/C%20ABI-1-blue)](#api)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

</div>

## Part Of The Same Toolkit

> Production-grade ExtendScript infrastructure for Illustrator-era JavaScript engines.

<table>
<tr>
<td width="50%" valign="top">

### Runtime Primitives

**[ESON](https://github.com/thelabcorner/eson)**  
Strict RFC 8259 JSON for ExtendScript.

**[ESB64](https://github.com/thelabcorner/es-b64)**  
Base64 and UTF-8 utilities.

**[ESARR](https://github.com/thelabcorner/es-arr)**  
ES5+ Array compatibility methods.

**[ESSTR](https://github.com/thelabcorner/es-str)**  
String whitespace and trim methods.

**[ESCHARS](https://github.com/thelabcorner/es-chars)**  
Native bulk byte operations.

**[ESHTTP](https://github.com/thelabcorner/es-http)**  
HTTP transport for ExtendScript automation.

**[ESTIMER](https://github.com/thelabcorner/es-timer)**  
Microsecond timing for ExtendScript automation.

</td>
<td width="50%" valign="top">

### Build & Integration Tools

**[ESPACK](https://github.com/thelabcorner/espack)**  
Self-extracting ExternalObject bundles.

**[ESMIN](https://github.com/thelabcorner/es-min)**  
Minification for shipped JSX bundles.

**[ESABI](https://github.com/thelabcorner/esabi)**  
Modern ExternalObject ABI declarations for native integrations.

**[VectorIPC](https://github.com/thelabcorner/vector-ipc)**  
Bounded local IPC for scripting hosts and native plug-ins.

**ESOBF** <sub>coming soon</sub>  
Obfuscation for hardened JSX distribution.

</td>
</tr>
</table>

Also from the same team: **[ArcFit.dev](https://arcfit.dev)**, deterministic arc warp for Illustrator.

---

## Table of Contents

- [Why VectorIPC?](#why-vectoripc)
- [Features](#features)
- [Which artifact should I use?](#which-artifact-should-i-use)
- [Get the Release](#get-the-release)
- [Installation](#installation)
- [Quick Start](#quick-start)
- [API](#api)
- [Validation](#validation)
- [Performance](#performance)
- [Security Model](#security-model)
- [Compatibility](#compatibility)
- [Engine quirks that shaped the design](#engine-quirks-that-shaped-the-design)
- [Development](#development)
- [Repository layout](#repository-layout)
- [Known limitations](#known-limitations)
- [Credits](#credits)
- [License](#license)

---

## Why VectorIPC?

Adobe scripting and native plug-ins frequently need work that should not live in the host process: persistence, networking, compression, expensive parsing, long-running services, or crash-isolated native code. Reimplementing that boundary per product means reimplementing framing, deadlines, cancellation, connection lifetime, security, and host-specific adapters every time.

VectorIPC separates those concerns:

```text
ExtendScript / ExternalObject ─┐
                              ├── VectorIPC ── out-of-process helper
Native plug-in / C / C++ ─────┘
```

The transport owns local IPC mechanics. Applications own operation IDs, payload schemas, serializers, helper lifecycle, and business logic.

VectorIPC v0.1.1 is intentionally small: one fixed binary envelope, one public C ABI, persistent Windows named pipes, a header-only C++ ownership layer, and one ExtendScript ExternalObject adapter. The wire contains opaque bytes; JSON, ESON, CBOR, protobuf, flat structs, or no payload at all are application choices.

---

## Features

- **Fixed 32-byte wire envelope** — explicit little-endian encode/decode under `VIPC/1.0`; the in-memory ABI descriptor is separate from wire layout.
- **Stable C ABI v1** — fixed-width public scalar types with compile-time size/offset assertions for `vipc_error` and `vipc_message`.
- **C++17 ownership façade** — move-only `vectoripc::Channel`, `Server`, and non-throwing `Error`; no second runtime or protocol.
- **Persistent full-duplex channels** — one send and one receive may run concurrently; same-direction overlap returns `VIPC_ERR_BUSY`.
- **Bounded operations** — one deadline covers the whole send/receive/accept operation; ambiguous timeout/cancellation states poison the channel instead of risking frame desynchronization.
- **16 MiB native wire limit** — exact-boundary tests exercise payload sizes through `VIPC_MAX_PAYLOAD_BYTES`.
- **Local Windows security boundary** — current-user DACL, `PIPE_REJECT_REMOTE_CLIENTS`, Windows-session-scoped endpoints, and peer PID/session metadata.
- **Recoverable listener state** — transient clients that connect and disappear before accept cannot permanently strand the server listener.
- **ExternalObject adapter v3** — up to 16 generation-tagged logical sessions in one loaded DLL, independent channels/staging buffers, stale-handle rejection; its host ABI is supplied by [ESABI v0.3.0](https://github.com/thelabcorner/esabi/releases/tag/v0.3.0).
- **ExtendScript wrapper v5** — dependency-free core wrapper with optional ESON, ESB64, and ESCHARS composition.
- **Release reproducibility** — `npm run release:manifest` emits exact commit/tag metadata, per-file and aggregate SHA-256 digests, and a tagged source archive.

---

## Which artifact should I use?

| | Native C | Native C++ | ExtendScript |
|---|---|---|---|
| Primary surface | `<vectoripc/vipc.h>` | `<vectoripc/vipc.hpp>` | `VectorIPC.jsx` |
| Binary | `vectoripc.lib` | same `vectoripc.lib` | `VectorIPCExternalObject.dll` |
| Runtime model | direct C ABI | header-only RAII over C ABI | synchronous ExternalObject calls |
| Serialization | application-owned | application-owned | application-owned; optional ESON helpers |
| Best for | helpers, plug-ins, native tools | C++ plug-ins/helpers | Illustrator ExtendScript |

**Rule of thumb:** share the service endpoint when components belong to the same application, but give each client its own channel.

---

## Get the Release

**[VectorIPC v0.1.1](https://github.com/thelabcorner/vector-ipc/releases/tag/v0.1.1)** is the current public pre-release.

Release assets include:

- `vector-ipc-v0.1.1-windows-x64.zip` — installed Windows x64 package: static library, C/C++ headers, CMake package files, ExternalObject DLL/wrapper, and MIT license;
- `vector-ipc-v0.1.1-source.zip` — deterministic tagged source archive;
- `vector-ipc-v0.1.1.lock.json` — exact commit, compatibility versions, and per-file/aggregate SHA-256 digests;
- `SHA256SUMS.txt` — checksums for the published lock, source archive, and Windows package.

For source consumers, pin `v0.1.1` rather than tracking the mutable `main` branch.

---

## Installation

### CMake dependency

```cmake
include(FetchContent)

set(VIPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(VIPC_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(VIPC_BUILD_EXTERNALOBJECT OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    vectoripc
    GIT_REPOSITORY https://github.com/thelabcorner/vector-ipc.git
    GIT_TAG v0.1.1
)
FetchContent_MakeAvailable(vectoripc)

target_link_libraries(my_target PRIVATE VectorIPC::vectoripc)
```

For an installed package:

```cmake
find_package(VectorIPC 0.1 CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE VectorIPC::vectoripc)
```

### ExtendScript adapter

Build with `VIPC_BUILD_EXTERNALOBJECT=ON` (the default on Windows). The adapter accepts installed ESABI exactly at `0.3.0`; otherwise it fetches the peeled v0.3.0 commit `3e99040c43cef573b477ad372b2a3a96c4d3a7d7` as a private build dependency. Then deploy:

```text
build/Release/VectorIPCExternalObject.dll
src/adapters/externalobject/VectorIPC.jsx
```

The wrapper remains dependency-free. ESON, ESB64, and ESCHARS are optional codecs used only when explicitly supplied or already loaded.

---

## Quick Start

### C++ client

```cpp
#include <vectoripc/vipc.hpp>

#include <array>
#include <cstdint>

int main() {
    vectoripc::Channel channel;
    vectoripc::Error error;

    if (vectoripc::Channel::connect(
            "my-helper",
            2000,
            channel,
            &error) != VIPC_OK) {
        return 1;
    }

    const std::array<std::uint8_t, 4> request{{1, 2, 3, 4}};

    vectoripc::Message message{};
    message.kind = VIPC_KIND_REQUEST;
    message.flags = VIPC_FLAG_NONE;
    message.operation = VIPC_APP_OPERATION_MIN;
    message.payload_size = static_cast<std::uint32_t>(request.size());
    message.correlation_id = 1;

    if (channel.send(message, request.data(), 2000, &error) != VIPC_OK) {
        return 1;
    }

    return 0;
}
```

### ExtendScript client

```javascript
$.evalFile(File("/absolute/path/to/VectorIPC.jsx"));

var ipc = VectorIPC.loadPath(
    "C:/absolute/path/to/VectorIPCExternalObject.dll"
);

ipc.connect("my-helper", 2000);

var response = ipc.requestBinaryString(
    0x100,
    String.fromCharCode(0, 1, 2, 255),
    2000
);

ipc.dispose();
```

For structured messages, `requestESON(..., ESON)` composes with ESON without making ESON part of the transport.

---

## API

### Version domains

| Surface | v0.1.1 |
|---|---|
| Product version | `0.1.1` |
| C ABI | `1` |
| Wire protocol | `VIPC/1.0` |
| Wire header | 32 bytes |
| Native max payload | 16 MiB |
| ExternalObject adapter | `3` |
| ExtendScript wrapper | `5` |
| ExternalObject max payload | 256 KiB |
| ExternalObject logical sessions | 16 |

These domains are deliberately independent. A product-version bump does not imply an ABI or wire break.

### C ABI

Core lifecycle and transport functions:

```c
vipc_server_create(...)
vipc_server_accept(...)
vipc_server_destroy(...)

vipc_client_connect(...)

vipc_channel_send(...)
vipc_channel_receive(...)
vipc_channel_destroy(...)
```

Endpoint tokens are application identifiers, not OS paths. Valid tokens use `[A-Za-z0-9._-]`; Windows maps them to:

```text
\\.\pipe\VectorIPC.<windows-session-id>.<endpoint>
```

Application operation IDs begin at `VIPC_APP_OPERATION_MIN` (`0x00000100`). VectorIPC reserves `0x00000000..0x000000FF` for transport/runtime control.

See [docs/PROTOCOL.md](docs/PROTOCOL.md), [docs/NATIVE.md](docs/NATIVE.md), and [docs/EXTERNALOBJECT.md](docs/EXTERNALOBJECT.md).

---

## Validation

The v0.1.1 candidate was qualified on Windows x64 with MSVC 19.44.35228 and live Adobe Illustrator 30.6.0 build 109R.

| Check | Command | Result |
|---|---|---|
| ES3 wrapper + native Release suite + installed package consumers | `npm test` | pass; 11/11 CTest targets, C11 and C++17 installed consumers |
| Protocol fuzz | included in `npm test` | 250,000 valid round trips + 250,000 mutated 32-byte headers |
| MSVC static analysis | `npm run test:analyze` | pass |
| AddressSanitizer | `npm run test:asan` | pass; 11/11 targets |
| Exact payload boundary | `npm run test:stress` | pass through 16 MiB |
| Timeout/cancellation resources | `npm run test:stress` | 2,000 timeout cycles, handles 59 → 59 |
| Accept/write cancellation | `npm run test:stress` | 2,000 accepts + 500 blocked writes, handles 59 → 59 |
| Full duplex | `npm run test:stress` | 100,000 messages in each direction |
| Small-frame soak | `npm run test:stress` | 1,000,000 verified frames |
| Illustrator integration | `npm run verify:live` | adapter v3 / wrapper v5 / raw bytes / sessions / ESON + ESB64 + ESCHARS pass |

The protocol decoder is transactional: failed header validation leaves the caller's output descriptor untouched. The mutation fuzzer checks that property across the invalid-header corpus.

---

## Performance

Native measurements below are from the documented Windows test host: AMD Ryzen 9 5900X, Windows 11 Pro build 22631, Release x64, MSVC 19.44.35228, Windows SDK 10.0.26100.0. Each case uses a separate helper process and one persistent VectorIPC channel.

| Payload each way | Median RTT | p95 | Duplex user-payload throughput |
|---:|---:|---:|---:|
| 0 B | 10.2 µs | 12.7 µs | — |
| 1 KiB | 20.0 µs | 23.7 µs | 91.6 MiB/s |
| 16 KiB | 20.8 µs | 35.1 µs | 1.40 GiB/s |
| 64 KiB | 22.0 µs | 43.1 µs | 4.69 GiB/s |
| 256 KiB | 63.8 µs | 111.8 µs | 6.57 GiB/s |
| 1 MiB | 432.6 µs | 621.9 µs | 4.24 GiB/s |
| 4 MiB | 1.480 ms | 2.141 ms | 4.81 GiB/s |

Helper-process spawn → connected measured **27.575 ms median-of-three-run-medians**. A separate repeated 1 KiB baseline measured **12.8 µs median RTT** and **124.8 MiB/s median duplex payload**, illustrating the scheduler sensitivity of microsecond-scale local IPC measurements.

Live Illustrator 30.6.0 measurements include the ExternalObject boundary:

| Path | Payload | Measured result |
|---|---:|---:|
| raw adapter v3 round trip | 7 B | 19 µs median-of-run-medians |
| hardened UTF-8 `requestText` echo | 64 KiB | 1.15 ms median-of-run-medians |
| hardened UTF-8 `requestText` echo | 256 KiB | 4.44 ms median-of-run-medians |

Representation dominates large ExtendScript payloads: the documented 64 KiB numeric-array pack measured about **4.0 s**, versus about **0.11 s** for the binary-string path. See [docs/BENCHMARKS.md](docs/BENCHMARKS.md) for methodology, repeated runs, and scripting-lane comparisons.

---

## Security Model

VectorIPC's v0.1.1 Windows trust boundary is local OS identity, not application authentication:

- named pipes use a DACL restricted to the current user;
- `PIPE_REJECT_REMOTE_CLIENTS` rejects remote clients;
- endpoint names are scoped by Windows session;
- clients request `SecurityIdentification`, not impersonation;
- peer PID and session metadata are exposed to the application;
- malformed frames, unknown flags, invalid versions, oversized payloads, and truncated streams fail closed;
- deadlines bound host-visible waits, and uncertain cancellation states poison the affected channel.

A different process running as the **same user in the same session remains inside that OS trust boundary**. Applications requiring stronger peer identity should add an application-level handshake, capability/build check, per-launch nonce, or equivalent policy above VectorIPC.

Payload bytes are untrusted application data. VectorIPC does not deserialize, evaluate, or execute them.

---

## Compatibility

| Target | Status |
|---|---|
| Windows x64 native C11 | supported and release-tested |
| Header-only C++17 façade | supported and release-tested |
| CMake install / `find_package(VectorIPC 0.1)` | supported; C11 + C++17 consumers tested |
| Adobe Illustrator 30.6.0 ExternalObject | live-certified |
| ESON / ESB64 / ESCHARS composition | live-certified in Illustrator 30.6.0 |
| Native Adobe `.aip` integration | same C/C++ ABI intended for direct linking; not separately host-certified in v0.1.1 |
| macOS / POSIX | explicit unsupported stub in v0.1.1; Unix-domain transport planned |
| 32-bit Windows | not a v0.1.1 release target |

The generated CMake package uses `SameMinorVersion`: a 0.1.x package satisfies a 0.1 request, while the package smoke test proves that 0.1.1 does **not** satisfy a 0.2 request.

---

## Engine quirks that shaped the design

All host-specific observations below were reproduced in Adobe Illustrator 30.6.0.

**ExternalObject DLLs can remain mapped after `unload()`.** Live verification therefore never loads the canonical build output directly. Each probe stages a uniquely named content copy under the user's temporary `vector-ipc-live` directory, so Illustrator module caching cannot lock or pollute the repository/build tree.

**Large ES3 numeric arrays are an expensive bulk representation.** At 64 KiB, numeric-array packing measured about 4.0 s on the documented host; binary-string packing measured about 0.11 s. VectorIPC keeps arbitrary binary on the wire and exposes binary-string/text paths so callers are not forced through indexed numeric arrays.

**Invalid UTF-16 can be silently normalized at the host boundary.** A live probe showed Illustrator 30.6.0 normalize a lone surrogate into an empty ExternalObject string. The wrapper validates Unicode scalar structure before crossing that boundary.

**ExternalObject method binding is a real compatibility surface.** Repository probes observed selective/reproducible binding failures per DLL build. The adapter therefore exposes one numeric `vipc(command, ...)` method and multiplexes commands behind that single binding.

---

## Development

Requirements for the certified Windows path:

- CMake 3.20+
- Visual Studio 2022 Build Tools with the x64 C/C++ toolchain
- Node.js for repository tooling
- Python only for live Illustrator probe orchestration

```powershell
npm test
npm run test:analyze
npm run test:asan
npm run test:stress
npm run verify

# Requires a running Illustrator instance:
npm run verify:live

# Measurements, not release thresholds:
npm run bench
npm run bench:matrix
npm run bench:live
npm run bench:live:matrix
npm run bench:live:wrapper
npm run bench:live:text -- --size 65536 --samples 5
npm run bench:live:decode
```

Release locks are generated only from a clean, correctly tagged tree:

```powershell
npm run release:manifest
```

The manifest records the exact commit/tag, ABI/protocol/adapter/wrapper versions, per-file and aggregate SHA-256 digests, and a tagged source archive.

---

## Repository layout

```text
include/vectoripc/                  public C/C++ headers
src/core/                           protocol/status core
src/platform/win/                   Windows named-pipe transport
src/platform/posix/                 explicit v0.1 unsupported stub
src/adapters/externalobject/        native adapter + ES3 wrapper
tests/                              unit, fuzz, boundary, stress, package tests
benchmarks/                         native transport benchmarks
tools/                              build, analysis, live probes, release tooling
docs/                               protocol, architecture, adapter, benchmark docs
```

Durable design decisions are recorded in [docs/DECISIONS.md](docs/DECISIONS.md).

---

## Known limitations

- v0.1.1 implements the transport only on Windows; the POSIX source is intentionally an unsupported stub.
- `vipc_channel_destroy()` and `vipc_server_destroy()` are not cross-thread cancellation APIs. Use finite deadlines, let in-flight work return, join the owning worker, then destroy the object.
- The ExternalObject adapter is synchronous at the Illustrator call boundary even though its logical sessions own independent native channels.
- The ExternalObject adapter caps payloads at 256 KiB even though the native wire cap is 16 MiB.
- Large legacy ExtendScript numeric arrays remain expensive; text/binary-string representations are the preferred bulk paths.
- Helper spawning, helper updates, authentication handshakes, application serializers, and product lifecycle policy are intentionally above the transport.

---

## Credits

VectorIPC's Adobe-host integration builds on the public ExtendScript/ExternalObject ecosystem documented by [docsforadobe](https://extendscript.docsforadobe.dev/) and Adobe's scripting interfaces. The Windows transport is built on documented Win32 named-pipe, overlapped-I/O, ACL, and process/session APIs.

[ArcFit.dev](https://arcfit.dev) provided the public production precedent for isolating Illustrator work behind a bounded native/helper boundary; VectorIPC generalizes that pattern into a product-neutral transport rather than reusing product-specific protocol code.

---

## License

MIT. See [LICENSE](LICENSE).
