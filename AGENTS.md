# AGENTS.md — VectorIPC

VectorIPC is foundational infrastructure. Keep the core smaller than its consumers.

## Required evidence

Before changing an Adobe-facing adapter, read the repository-root skills that
govern that boundary:

- `agent-skills/externalobject-extendscript/SKILL.md`
- `agent-skills/adobe-illustrator-scripting/SKILL.md`
- `agent-skills/illustrator-com-automation-skill/SKILL.md` for live Illustrator
  verification
- native plug-in guidance in
  `illustrator-com-automation-skill/references/actions-events-and-plugins.md`

UXP is not a VectorIPC transport target unless explicitly requested. Illustrator
UXP is an internal/version-sensitive surface and must not be treated as a public
native plug-in replacement.

## Architecture rules

1. `src/core/` is platform-independent. No Win32, Adobe, Illustrator, or
   ExtendScript symbols.
2. `src/platform/` implements local byte-stream transports. Product protocols
   do not live there.
3. `src/adapters/` translates a host ABI into VectorIPC. Adapters may not redefine
   the wire format.
4. Anything on the wire is defined once in
   `include/vectoripc/vipc_protocol.h` and documented in `docs/PROTOCOL.md`.
5. Payload bytes are opaque. Do not add JSON, CBOR, ESON, or product schemas to
   core.
6. Every blocking operation takes a finite timeout. No unbounded application wait.
7. Every inbound length is untrusted and bounded before copy/allocation.
8. Transport failures and product/application failures are separate domains.
9. A channel permits at most one concurrent reader and one concurrent writer.
   Same-direction concurrency must be serialized above the transport.
10. Unknown/malformed frames fail closed. Never silently reinterpret an
    incompatible frame.
11. Do not put Illustrator SDK calls on an VectorIPC transport thread.
12. Do not put product business logic inside an ExternalObject DLL.

## ExternalObject rules

The ExternalObject boundary is legacy, synchronous, and version-sensitive:

- use the canonical Adobe `TaggedData` ABI and 8-byte packing;
- keep a small, flat API;
- returned strings are UTF-8, NUL-terminated, and owned exactly as
  `ESFreeMem` specifies;
- never return fatal negative `kESErr*` method results;
- do not expose raw pointers or object graphs;
- do not assume raw arbitrary bytes survive `kTypeString` (NUL truncation and
  surrogate-window limitations are measured);
- treat host-bypass failures as possible even through JSX `try/catch`;
- verify a fresh DLL build in a fresh Illustrator process before calling it
  runtime-certified.

## Performance

- Header encode/decode is allocation-free.
- Persistent channels are the primary path.
- Do not copy payload bytes merely to serialize the envelope.
- Benchmark warm latency, cold connection, throughput, CPU, allocation count,
  and binary size.
- Report environment, warmups, sample count, median, p95, and correctness gate.
- Never optimize away validation.

## Security

- Windows named pipes use `PIPE_REJECT_REMOTE_CLIENTS`.
- The server uses an explicit current-user-only DACL; never rely on the default
  named-pipe DACL.
- Clients open the pipe with `SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION`
  so the helper cannot impersonate the Adobe host token.
- Endpoint components use a conservative ASCII token grammar.
- Every accepted peer is treated as untrusted even after transport ACL checks.
- Product helpers still authenticate/validate their own application handshake.

## Validation

Run `npm test` before claiming transport changes are complete. Add adversarial
coverage for malformed headers, oversized payloads, truncated streams, timeouts,
partial I/O, disconnects, bad versions, and buffer exhaustion.

Static/native success is not Illustrator runtime success. ExternalObject claims
require a live Illustrator probe via the COM skill.