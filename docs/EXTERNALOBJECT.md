# ExtendScript ExternalObject Adapter

Status: **adapter v3 / wrapper v5 — Windows + live Illustrator 30.6.0 certified**

The ExternalObject adapter is a synchronous host façade over the VectorIPC C API.
It is deliberately **not** a second VectorIPC wire protocol.

## Architecture

```text
ExtendScript / ES3
      │
      │ one method: vipc(command, ...)
      │ numeric ingress / ASCII string egress
      ▼
VectorIPCExternalObject.dll
      │
      │ normal VectorIPC binary frames
      ▼
persistent local helper process
```

The DLL contains no product-specific document, serializer, database, network,
or other business logic.

## ABI dependency

The adapter uses [ESABI v0.3.0](https://github.com/thelabcorner/esabi/releases/tag/v0.3.0) as the single definition of the ExtendScript ExternalObject binary boundary. VectorIPC no longer carries its own TaggedData layout, tag table, lifecycle signatures, or packing declarations.

CMake first accepts an installed `esabi::esabi` package only when it is exactly version `0.3.0`. If one is not present, the ExternalObject build fetches the peeled `v0.3.0` commit `3e99040c43cef573b477ad372b2a3a96c4d3a7d7` and adds it as an `EXCLUDE_FROM_ALL` private build dependency. ESABI's own install rules are therefore excluded from VectorIPC's install/package surface. The small `vipc_externalobject_abi.h` file is now only a VectorIPC naming façade over ESABI types/constants.

This migration also corrected the adapter's historical `ESInitialize` declaration from a pointer-to-pointer argument to the documented single value-array pointer. The adapter never dereferenced that argument, so wire/host behavior is unchanged; the declaration now matches the actual ABI contract.

## Host surface

`ESInitialize` registers exactly one script-visible method:

```text
vipc
```

`ESGetVersion()` returns **3**.

Every `vipc(...)` call returns `kESErrOK`. Success, validation failures,
state failures, and transport failures are data returned through an ASCII
`kTypeString`. VectorIPC does not intentionally emit fatal negative
`kESErr*` results.

This one-method design is intentional. Repository measurements showed that
ExternalObject method binding can fail selectively and reproducibly per DLL
build. A numeric command multiplexer makes one binding the entire critical host
surface.

## Adapter v3: logical sessions + text transaction

Adapter v3 retains the v2 generation-tagged session model: up to **16
independent logical sessions** inside one loaded DLL. It additionally provides a
validated NUL-free UTF-8 text transaction while keeping arbitrary binary on the
numeric/Base64-safe path.

Each CONNECT allocates a generation-tagged numeric handle:

```text
handle = (generation << 8) | (slot + 1)
```

The implementation currently reserves eight low bits for the slot token and the
remaining 24 bits for generation. A handle is validated against both its slot
and generation before any staged payload or channel state can be touched.

Consequences:

- two scripts/clients can hold independent channels and independent staged
  payloads through the same loaded DLL;
- closing one session does not mutate another session;
- a closed handle immediately becomes stale;
- a later session allocated into the same slot receives a different generation
  token;
- the 17th simultaneous CONNECT fails with
  `VIPC/1.0|ERR|STATE|SESSION_LIMIT`.

This is **state isolation**, not parallel ExternalObject execution. The DLL still
serializes the single `vipc(...)` entry point with a small global busy guard.
That matches ExtendScript's synchronous host boundary while keeping per-client
transport state independent.

## Numeric byte packing

ExternalObject string arguments are not trusted as a reliable ingress lane.
Arbitrary bytes are packed little-endian, six per JavaScript Number:

```text
packed =
    b0
  + b1 * 2^8
  + b2 * 2^16
  + b3 * 2^24
  + b4 * 2^32
  + b5 * 2^40
```

The largest packed value is `2^48 - 1`, comfortably inside IEEE-754's exact
integer range. Every possible byte value—including NUL and 0x80–0xFF—therefore
survives the numeric lane exactly.

The explicit byte count is authoritative for the final one-to-six-byte packed
word.

## Command contract

The first argument is always the command ID.

| ID | Command | Arguments after ID | Result |
|---:|---|---|---|
| 0 | INFO | none | adapter/wire/capability metadata |
| 1 | CONNECT | `timeoutMs, endpointBytes, packedEndpoint...` | allocates session and connects |
| 2 | STAGE_RESET | `handle` | clears that session's staged payload |
| 3 | STAGE_APPEND | `handle, byteCount, packedBytes...` | appends bytes to that session |
| 4 | TRANSACT | `handle, timeoutMs, operation, corrLow, corrHigh` | REQUEST + correlated RESPONSE |
| 5 | SEND | `handle, kind, flags, operation, corrLow, corrHigh, timeoutMs` | generic framed send |
| 6 | RECEIVE | `handle, timeoutMs` | receives one frame |
| 7 | CLOSE | `handle` | releases session/channel/buffers |
| 8 | TRANSACT_TEXT | `handle, timeoutMs, operation, corrLow, corrHigh, text` | UTF-8 REQUEST + validated UTF-8 RESPONSE |

The staged payload is retained after SEND/TRANSACT so a caller can intentionally
reuse it. STAGE_RESET begins a new staged payload.

Before TRANSACT puts a request on the wire, it reserves the receive buffer. An
allocation failure therefore cannot leave an unread response behind and
desynchronize the stream.

## Response grammar

INFO:

```text
VIPC/1.0|INFO|adapterVersion|wireMajor|wireMinor|maxPayload|packBytes|maxSessions
```

Current exact value:

```text
VIPC/1.0|INFO|3|1|0|262144|6|16
```

Simple success:

```text
VIPC/1.0|OK|...
```

CONNECT:

```text
VIPC/1.0|OK|CONNECTED|handle|peerPid|peerSessionId
```

Received frame:

```text
VIPC/1.0|FRAME|kind|flags|operation|corrLow|corrHigh|payloadBytes|base64Payload
```

Validated text response:

```text
VIPC/1.0|TEXT|kind|flags|operation|corrLow|corrHigh|payloadBytes|utf8Payload
```

`TRANSACT_TEXT` rejects embedded NUL, invalid UTF-8 request/response data, and
payloads beyond the 256 KiB adapter cap. It is a text contract, not a raw-binary
transport.

Native transport error:

```text
VIPC/1.0|ERR|status|phase|platformCode
```

Adapter/state errors remain descriptive ASCII, for example:

```text
VIPC/1.0|ERR|ARGS|CONNECT
VIPC/1.0|ERR|STATE|INVALID_HANDLE
VIPC/1.0|ERR|STATE|SESSION_LIMIT
VIPC/1.0|ERR|PROTOCOL|CORRELATION
```

TRANSACT verifies RESPONSE kind, operation ID, and all 64 correlation bits.
A mismatch releases that session rather than allowing a synchronous caller to
continue on a potentially desynchronized stream.

## One composite deadline

TRANSACT is one host-visible operation. Its send and receive legs consume one
total timeout budget:

```text
start
  ├─ send
  └─ receive(remaining timeout)
```

A nominal 2-second call therefore cannot silently turn into roughly 4 seconds
because send and receive each received a fresh allowance.

Transport failures that poison/close the channel release the logical session.
The caller reconnects rather than reusing an uncertain stream.

## Why the adapter cap is 256 KiB

The native VectorIPC wire allows payloads through **16 MiB**. The legacy
ExternalObject surface intentionally caps each session's staged/received payload
at **256 KiB**.

Base64 expansion at the cap is:

```text
262,144 bytes → 349,528 ASCII characters
```

plus a small FRAME prefix. That remains below the roughly 360 KiB
`kTypeString` return range already exercised by this repository's Illustrator
ExternalObject research.

The 256 KiB limit is a host-safety boundary, not a transport limitation. Native
SDK consumers retain the normal 16 MiB wire ceiling. Large artifacts should
normally travel through product-owned files/CAS rather than through ExtendScript.

## Canonical ES3 wrapper

There is one public scripting implementation:

```text
src/adapters/externalobject/VectorIPC.jsx
```

`VectorIPC.jsxinc` is shipped as a byte-identical include-friendly mirror.
`npm test` compares the two files before compiling anything and fails if they
diverge.

It exposes both names for compatibility:

```javascript
VectorIPC
VectorIPCExternalObject
```

They refer to the same object.

### Load

By native folder + base name:

```javascript
$.evalFile(File("/path/to/VectorIPC.jsx"));

var ipc = VectorIPC.load(
    "C:/my-product/native",
    "VectorIPCExternalObject"
);
```

Or by absolute DLL path:

```javascript
var ipc = VectorIPC.loadPath(
    "C:/my-product/native/VectorIPCExternalObject.dll"
);
```

Loading immediately negotiates and verifies:

- adapter version = 3;
- wire major = 1;
- packing width = 6 bytes;
- adapter payload cap = 262,144 bytes;
- at least one logical session.

A stale/incompatible DLL therefore fails before a product request is sent.

### Connect and request

```javascript
ipc.connect("my-helper", 2000);

var frame = ipc.requestByteArray(
    0x100,
    [0, 1, 2, 255],
    2000
);

var bytes = VectorIPC.decodeBase64(frame.payloadBase64);
ipc.dispose();
```

Available client methods include:

- `info()`
- `connect(endpoint, timeoutMs)`
- `stageReset()`
- `stageByteArray(bytes, chunkWords)`
- `stageBinaryString(value, chunkWords)`
- `transact(operation, corrLow, corrHigh, timeoutMs)`
- `requestByteArray(operation, bytes, timeoutMs, correlation, chunkWords)`
- `requestBinaryString(operation, value, timeoutMs, correlation, chunkWords)`
- `requestText(operation, value, timeoutMs, correlation)`
- `requestUtf8(...)` — alias of `requestText`
- `requestESON(operation, value, timeoutMs, correlation, codec)`
- `requestJSON(...)` — alias of `requestESON`
- `send(...)`
- `receive(timeoutMs)`
- `nextCorrelation()`
- `close()`
- `dispose()`

The default and hard maximum staging chunk is
**256 packed words = 1,536 bytes per host call**. The chunk size can be
overridden downward for testing or product-specific latency/memory tradeoffs.
Values above 256 are rejected before `STAGE_RESET`, which bounds the synchronous
`Function.apply` argument surface to the live-certified range.

Correlation IDs are represented as two exact uint32 values:

```javascript
{ low: 2882400001, high: 305419896 }
```

The wrapper's automatic generator also maintains the low/high pair explicitly;
it never combines an arbitrary 64-bit correlation ID into one JavaScript Number.

The wrapper exposes three response representations:

- `decodeBase64(text)` → numeric byte array; exact compatibility path;
- `decodeBase64BinaryString(text, codec)` → ESB64-compatible binary string;
- `decodeBase64Hex(text, codec)` → ESCHARS-compatible hexadecimal string.

`requestESON/requestJSON`, `decodeBase64BinaryString`, and
`decodeBase64Hex` are **optional composition helpers**. VectorIPC neither
bundles nor requires ESON/ESB64/ESCHARS; a compatible codec is passed explicitly
or resolved from an already-loaded global.

### Performance-first representation guidance

Live Illustrator profiling makes the preferred order concrete:

1. structured/control data → `requestESON/requestJSON`;
2. NUL-free UTF-8 → `requestText`;
3. arbitrary bytes already represented as a JS string →
   `requestBinaryString`;
4. arbitrary-byte response that can remain a string →
   `decodeBase64BinaryString` with ESB64;
5. hex consumer → `decodeBase64Hex` with ESCHARS;
6. numeric byte arrays only when the caller actually requires numeric elements.

On this host, 64 KiB numeric-array packing measured roughly **4.0 s**, while
binary-string packing measured roughly **0.11 s**. Converting an existing numeric
array to a string with `slice + String.fromCharCode.apply` still cost
approximately **3.8–5.9 s** at 64 KiB, so it is not a hidden shortcut.

## Native test evidence

Current Release gate:

```powershell
npm test
```

passes the ES3 wrapper unit suite, **11/11 CTest targets**, and an installed
out-of-tree CMake consumer. Coverage includes:

- core/adversarial selftest;
- 250,000 valid + 250,000 mutated protocol headers;
- full-duplex transport and same-direction busy rejection;
- 20,000-frame variable-size transport stress;
- exact payload boundaries through the 16 MiB wire maximum;
- timeout and cancellation stress;
- concurrent full-duplex stress;
- C++17 ownership façade;
- ExternalObject ABI/binary/text transaction behavior;
- stale-handle and 16-session generation isolation.

The multi-session test additionally:

- stages different payloads in two sessions before either transaction;
- services them in reversed order;
- closes one and proves the other survives unchanged;
- rejects the stale closed handle;
- fills all 16 session slots;
- rejects the 17th with SESSION_LIMIT;
- frees a slot and proves the replacement generation handle differs.

The dedicated long stress gate:

```powershell
npm run test:stress
```

has also passed:

- **2,000** forced timeout/cancellation cycles with Windows process handle count
  unchanged (**59 → 59**);
- **100,000 messages in each direction** concurrently on one full-duplex
  channel;
- **1,000,000** verified 0–4 KiB request/response frames;
- **50,000** verified large frames through 256 KiB at ~**1.56 GiB/s** duplex
  user payload on this workstation.

The current MSVC AddressSanitizer build passes the native suite with no reported
memory-safety findings.

## Live Illustrator 30.6.0 certification

The live probes use the repository's serialized Illustrator COM automation tool,
zero open documents, and disposable numbered DLL copies staged in the current
user's temporary `vector-ipc-live` directory rather than the repository.

Current commands:

```powershell
npm run test:live:info
npm run test:live:raw
npm run test:live
npm run test:live:sessions
npm run test:live:ecosystem
npm run bench:live
```

Verified live:

1. the single `vipc` method binds;
2. direct invocation and `Function.apply` return identical v3 INFO;
3. numeric endpoint and payload packing arrive byte-perfect;
4. NUL, 0xFF, 0x80, and 0x7F survive a real helper round trip;
5. a generation-tagged session handle is allocated and used by every
   session-scoped command;
6. stale-handle use is rejected after CLOSE;
7. Base64 egress is byte-correct;
8. the canonical wrapper negotiates adapter v3 / wrapper v5 before connecting;
9. a 1,024-byte wrapper request validates every returned byte;
10. that wrapper probe forces 128 packed words (768 bytes) per stage call, so
    the 1 KiB payload crosses the multi-call staging path;
11. two logical sessions retain independent channels/staged data under
    interleaved use and one-client disposal;
12. direct UTF-8 text round-trips Unicode correctly;
13. the actual ESON, ESB64 accelerator, and ESCHARS accelerator compose in the
    same live session. The current ecosystem probe returns:

```text
PASS|wrapper=5|adapter=3|eson=ok|esb64=native|eschars=native|hex=000102ff10807f
```

### Adapter-v3 raw ExternalObject latency

The live benchmark runs 200 warmups + 2,000 timed transactions per run. Its
timed interval covers:

```text
String(lib.vipc(TRANSACT...))
  = ExternalObject boundary
  + VectorIPC named-pipe request/response
  + correlation validation
  + Base64 return construction
  + conversion to ExtendScript string
```

Nine current adapter-v3 runs produced an overall median of run medians of
**19 µs**. Individual run medians ranged **19–25 µs**; the median run p95 was
**34 µs**. This is effectively unchanged from the earlier adapter-v2 floor.

The corresponding separate-process native transport baseline remains roughly
10–14 µs median warm RTT on this workstation. For tiny payloads, the full live
ExternalObject façade therefore adds only a small number of microseconds above
the raw process transport; for larger payloads, ExtendScript packing/decoding
quickly becomes the dominant cost.

### Bulk representation measurements

Measured live on the same Illustrator 30.6.0 host:

| Path | Payload | Median / representative time |
|---|---:|---:|
| hardened UTF-8 `requestText` echo | 64 KiB | **1.15 ms** median-of-run-medians |
| hardened UTF-8 `requestText` echo | 256 KiB | **4.44 ms** median-of-run-medians |
| numeric-array staging pack | 64 KiB | **~4.0 s** |
| binary-string staging pack | 64 KiB | **~0.11 s** |
| numeric-array Base64 decode | 65,535 B | **~1.9–2.3 s** |
| ESB64 native binary-string decode, NUL-free | 65,535 B | **~1.08–1.16 ms** |
| ESB64 exact NUL-safe ES3 fallback | 65,535 B | **~82–88 ms** |

The ESB64 result is why wrapper v5 provides
`decodeBase64BinaryString(...)`: even its NUL-safe fallback is roughly an order
of magnitude faster than materializing a large numeric array, and its native
lane is roughly three orders of magnitude faster on the measured NUL-free case.

The text figures include wrapper-side surrogate-pair validation. This is not
optional defensive ceremony: live Illustrator testing showed a lone surrogate
could cross the ExternalObject boundary as an empty string. The wrapper therefore
rejects unpaired UTF-16 surrogates before invoking native code while preserving
valid surrogate pairs/astral Unicode.

## DLL lifetime

All live probes call `unload()`. Illustrator nevertheless keeps the disposable
DLL file locked afterward on this host, matching prior repository measurements.

Therefore development/live certification must never load the canonical build
output directly or stage host-loaded copies in the repository:

1. copy the DLL contents to a unique numbered/name-suffixed path under the
   user-writable temporary `vector-ipc-live` directory;
2. load that disposable copy;
3. leave it alone if Illustrator keeps it pinned;
4. best-effort sweep old probe copies after Illustrator exits.

The live tools implement this automatically. They intentionally use a content-only
copy so the staged file inherits the temp directory's ownership/ACL rather than
source metadata from a checkout that may itself live under `Program Files`.
