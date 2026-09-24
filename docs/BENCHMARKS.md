# Benchmarks

## Windows persistent helper baseline — 2026-09-23

Command:

```powershell
npm run bench
```

Environment:

| Item | Value |
|---|---|
| CPU | AMD Ryzen 9 5900X 12-Core Processor |
| OS | Windows 11 Pro 10.0.22631 build 22631 |
| Build | Release x64 |
| Compiler | MSVC 19.44.35228 |
| Windows SDK | 10.0.26100.0 |
| Topology | parent client ↔ separate helper process, same Windows session |

Measured run:

| Case | n | Mean | Median | p95 | Throughput |
|---|---:|---:|---:|---:|---:|
| Helper process spawn → pipe connected | 25 | 27.669 ms | 29.256 ms | 29.885 ms | — |
| Warm round trip, 0-byte payload | 20,000 | 11.601 µs | 10.300 µs | 16.600 µs | 86,201 round trips/s |
| Warm round trip, 1,024 bytes each way | 10,000 | 14.377 µs | 12.600 µs | 20.600 µs | 69,558 round trips/s; 135.9 MiB/s duplex user payload |

Three consecutive follow-up runs on the same build showed the expected
microbenchmark sensitivity to scheduler/background load:

| Run | Spawn → connected median | Empty RTT median | Empty p95 | 1 KiB RTT median | 1 KiB p95 | 1 KiB duplex payload |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 23.596 ms | 10.2 µs | 15.5 µs | 12.3 µs | 20.4 µs | 142.3 MiB/s |
| 2 | 27.575 ms | 14.1 µs | 24.0 µs | 14.3 µs | 23.1 µs | 122.1 MiB/s |
| 3 | 29.319 ms | 10.3 µs | 17.7 µs | 12.8 µs | 23.0 µs | 124.8 MiB/s |

The median of those three run medians is **27.575 ms** helper spawn →
connected, **10.3 µs** for an empty warm request/response, and **12.8 µs** for
1 KiB each way. The median reported 1 KiB duplex throughput is **124.8 MiB/s**.
VectorIPC therefore keeps distributions and repeat runs rather than presenting a
single best-case measurement as the transport's universal latency.

The benchmark performs a correctness-checked echo over a real child process.
The warm measurements exclude process creation and reuse one persistent VectorIPC
connection. The helper-start measurement intentionally includes Windows process
creation, server initialization, endpoint creation, client retry/discovery, and
connection.

These numbers measure VectorIPC itself. They do **not** include ExternalObject,
ExtendScript string/base64 conversion, Illustrator SDK dispatch, a product
serializer, or product business logic.

## Native payload matrix — current Release build

Command:

```powershell
npm run bench:matrix
```

Separate helper process, one persistent channel:

| Payload each way | Median RTT | p95 | Duplex user-payload throughput |
|---:|---:|---:|---:|
| 0 B | 10.2 µs | 12.7 µs | — |
| 1 KiB | 20.0 µs | 23.7 µs | 91.6 MiB/s |
| 4 KiB | 22.6 µs | 30.0 µs | 322.6 MiB/s |
| 16 KiB | 20.8 µs | 35.1 µs | 1.40 GiB/s |
| 64 KiB | 22.0 µs | 43.1 µs | 4.69 GiB/s |
| 256 KiB | 63.8 µs | 111.8 µs | **6.57 GiB/s** |
| 1 MiB | 432.6 µs | 621.9 µs | 4.24 GiB/s |
| 4 MiB | 1.480 ms | 2.141 ms | 4.81 GiB/s |

The matrix demonstrates that VectorIPC's native transport is not the limiting
factor for the legacy scripting adapter. At the ExternalObject adapter's entire
256 KiB payload cap, the native binary round trip remains on the order of tens
of microseconds.

## Gate paired with the benchmark

The same Release build currently passes:

- protocol/core and timeout/adversarial selftest;
- 250,000 valid protocol round trips plus 250,000 mutated 32-byte headers;
- simultaneous read/write full-duplex behavior and same-direction busy guard;
- malformed/truncated raw-wire rejection, blocked-writer contention, and listener
  recovery after accept cancellation;
- 64 connect/request/response/close churn cycles;
- 20,000 variable-size (0–4,096 byte) persistent transport round trips;
- ExternalObject v3 ABI binary/text transaction + stale-handle rejection;
- ExternalObject v3 multi-session isolation, 16-slot limit, and generation reuse;
- C++17 façade and installed-package consumer.

Re-run both `npm test` and `npm run bench` after any transport, framing,
deadline, or security change before updating this document.

## Live Illustrator ExternalObject path — 2026-09-23

Host:

| Item | Value |
|---|---|
| Application | Adobe Illustrator 30.6.0 build 109R |
| Scripting version | 30.0 |
| Documents open | 0 during probe |
| DLL | numbered disposable copy of `VectorIPCExternalObject.dll` |
| Adapter | v3, 16 logical session slots; wrapper v5 |
| Topology | ExtendScript → ExternalObject → VectorIPC → separate helper process |
| Payload | 7 raw bytes: `00 01 02 FF 10 80 7F` |
| Warmup | 200 round trips |
| Samples | 2,000 per run |
| Timer | `$.hiresTimer`, primed immediately before each call |

The timed interval includes `String(lib.vipc(...))`, so it covers the actual
ExternalObject call, named-pipe request/response, correlation validation, Base64
return construction, and conversion to an ExtendScript string. Response
validation is performed **after** the timer read and is therefore excluded.

Nine current **adapter-v3** live runs from `npm run bench:live` produced an
overall median of run medians of **19 µs**. Individual run medians ranged
**19–25 µs**; the median run p95 was **34 µs**.

The live correctness probe independently verified:

- the sole `vipc` method binds as a function in Illustrator;
- numeric 48-bit packed arguments arrive intact;
- `00` and `FF` bytes round-trip without string-channel corruption;
- v3 INFO reports the exact `3|1|0|262144|6|16` capability contract;
- a generation-tagged logical session connects from inside the ExternalObject DLL;
- operation and 64-bit correlation identity survive the round trip;
- Base64 `kTypeString` return data is byte-correct;
- a closed session handle is rejected as stale;
- independent sessions retain independent channels/staged payloads when another
  session is closed;
- `ESFreeMem`/unload complete without document mutation.

The high-level 1 KiB wrapper gate forces a 768-byte stage chunk, guaranteeing
multi-call staging. The latest run measured **3 ms** for staging +
request/response + frame parsing and **5 ms** for the pure-ExtendScript Base64
decode + byte validation. These millisecond-clock values are intentionally
reported separately from the microsecond raw bridge benchmark.

### Bulk scripting-path measurements

Representation choice dominates large ExtendScript payloads:

| Live Illustrator path | Payload | Measured time |
|---|---:|---:|
| hardened UTF-8 `requestText` echo | 64 KiB | **1.15 ms median-of-run-medians** |
| hardened UTF-8 `requestText` echo | 256 KiB | **4.44 ms median-of-run-medians** |
| numeric-array pack | 64 KiB | **~4.0 s** |
| binary-string pack | 64 KiB | **~0.11 s** |
| numeric-array Base64 decode | 65,535 B | **~1.9–2.3 s** |
| ESB64 native binary-string decode | 65,535 B NUL-free | **1.08–1.16 ms** |
| ESB64 exact ES3 fallback | 65,535 B with NUL | **82–88 ms** |

Naively converting an existing numeric byte array to a binary string with
`slice + String.fromCharCode.apply` did not help: the conversion alone measured
approximately **3.8–5.9 s at 64 KiB**. The engine's indexed array access is the
binding cost.

The final text numbers include wrapper-side UTF-16 surrogate-pair validation.
That validation is intentional: a live boundary probe showed Illustrator 30.6.0
silently normalized a lone surrogate into an empty ExternalObject string instead
of rejecting it. Prevalidation prevents that silent data corruption.

The real live ecosystem certification also passed with the actual sibling
libraries loaded:

```text
PASS|wrapper=5|adapter=3|eson=ok|esb64=native|eschars=native|hex=000102ff10807f
```

Reproduce the host layers separately from the native benchmark:

```powershell
npm run bench:live
npm run test:live:ecosystem
```

These measurements are specific to this Windows/Illustrator build and tiny
payload. They establish the architecture's overhead floor, not a universal
latency guarantee.