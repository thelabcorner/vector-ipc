# VectorIPC Wire Protocol

Protocol version: **1.0**

## 1. Envelope

Every frame starts with exactly 32 bytes. All integers are unsigned,
little-endian.

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 4 | `magic` | ASCII `VIPC` |
| 4 | 2 | `major` | wire major version |
| 6 | 2 | `minor` | wire minor version |
| 8 | 2 | `headerSize` | `32` for v1 |
| 10 | 2 | `kind` | message kind |
| 12 | 4 | `flags` | transport-level flags |
| 16 | 4 | `operation` | product-defined operation ID |
| 20 | 8 | `correlationId` | request/event correlation |
| 28 | 4 | `payloadSize` | following payload byte count |

The payload immediately follows the header with no padding.

## 2. Message kinds

| Value | Name | Semantics |
|---:|---|---|
| 1 | `REQUEST` | expects a correlated response |
| 2 | `RESPONSE` | result for a request |
| 3 | `NOTIFY` | fire-and-forget client/server notification |
| 4 | `EVENT` | unsolicited event, normally helper → client |
| 5 | `CONTROL` | transport/runtime control plane reserved by VectorIPC |

Kinds `0` and values outside the defined range are invalid in wire v1.

## 3. Flags

Wire v1 defines:

- `VIPC_FLAG_NONE = 0`
- `VIPC_FLAG_ERROR = 1 << 0` — the payload is a product/application error
  response rather than a successful result.

All other bits are reserved and must be zero in v1. A receiver rejects unknown
bits rather than guessing their semantics.

## 4. Operation IDs

`0x00000000..0x000000FF` are reserved for VectorIPC control/runtime use.

`0x00000100..0xFFFFFFFF` are application-defined.

VectorIPC does not assign application-specific operations.

## 5. Correlation IDs

- `REQUEST`: non-zero correlation ID strongly recommended and required by
  higher-level multiplexers.
- `RESPONSE`: must repeat the request correlation ID.
- `NOTIFY`: may use zero.
- `EVENT`: may use zero when not associated with another operation.

VectorIPC transports the value but does not generate or globally interpret it.

## 6. Payloads

Payloads are opaque bytes. The protocol has no mandatory serializer.

The v1 library hard cap is `VIPC_MAX_PAYLOAD_BYTES` (16 MiB). Consumers should
choose substantially smaller product limits for their normal messages and move
large artifacts through product-owned files/CAS when appropriate.

## 7. Version compatibility

V1 receivers require:

- exact magic;
- major version `1`;
- minor version not greater than the receiver's supported minor;
- header size exactly 32;
- known message kind;
- no unknown flags;
- payload size at or below the hard cap.

A future incompatible envelope increments the major version.

Product/application version negotiation is a separate handshake in the payload
schema owned by the consumer.

## 8. Stream behavior

The transport is a byte stream even on Windows named pipes:

```text
[32-byte header][payload][32-byte header][payload]...
```

Readers must use exact-length reads and must never assume one OS read equals one
VectorIPC frame.

If the caller's receive buffer is smaller than a valid payload, VectorIPC drains
that payload within the same deadline and returns `VIPC_ERR_BUFFER_TOO_SMALL`.
This preserves stream synchronization for the next frame.

Malformed/oversized frames poison the channel because resynchronizing by scanning
for magic would turn attacker-controlled payload bytes into framing.