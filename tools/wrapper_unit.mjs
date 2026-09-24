import { readFileSync } from "node:fs";
import vm from "node:vm";
import { resolve } from "node:path";

const root = resolve(import.meta.dirname, "..");
const source = readFileSync(
  resolve(root, "src/adapters/externalobject/VectorIPC.jsx"),
  "utf8",
);

const context = vm.createContext({});
vm.runInContext(source, context, { filename: "VectorIPC.jsx" });

const VectorIPC = context.VectorIPC;
if (!VectorIPC) throw new Error("VectorIPC wrapper did not initialize");

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function bytesEqual(actual, expected, label) {
  assert(actual.length === expected.length, label + ": length");
  for (let i = 0; i < expected.length; i++) {
    assert(actual[i] === expected[i], label + ": byte " + i);
  }
}

function expectThrow(fn, contains, label) {
  let thrown = null;
  try {
    fn();
  } catch (error) {
    thrown = error;
  }
  assert(thrown, label + ": expected throw");
  assert(
    String(thrown).includes(contains),
    label + ": unexpected error " + String(thrown),
  );
}

const vectors = [
  ["", []],
  ["Zg==", [0x66]],
  ["Zm8=", [0x66, 0x6f]],
  ["Zm9v", [0x66, 0x6f, 0x6f]],
  ["AAEC/xCAfw==", [0x00, 0x01, 0x02, 0xff, 0x10, 0x80, 0x7f]],
  ["////", [0xff, 0xff, 0xff]],
];

for (const [encoded, expected] of vectors) {
  bytesEqual(VectorIPC.decodeBase64(encoded), expected, encoded || "empty");
}

let prngState = 0x6d2b79f5;
function nextByte() {
  prngState = (Math.imul(prngState, 1664525) + 1013904223) >>> 0;
  return prngState & 0xff;
}

for (let length = 0; length <= 511; length++) {
  const expected = new Array(length);
  for (let i = 0; i < length; i++) expected[i] = nextByte();
  const encoded = Buffer.from(expected).toString("base64");
  bytesEqual(
    VectorIPC.decodeBase64(encoded),
    expected,
    "differential Base64 length " + length,
  );
}

{
  const expected = new Array(262144);
  for (let i = 0; i < expected.length; i++) expected[i] = nextByte();
  const encoded = Buffer.from(expected).toString("base64");
  bytesEqual(
    VectorIPC.decodeBase64(encoded),
    expected,
    "max adapter payload Base64",
  );
}

expectThrow(
  () => VectorIPC.decodeBase64("A"),
  "Base64 length",
  "length rejection",
);
expectThrow(
  () => VectorIPC.decodeBase64("AA?="),
  "Base64 payload",
  "alphabet rejection",
);
expectThrow(
  () => VectorIPC.decodeBase64("A=AA"),
  "Base64 payload",
  "padding in second slot",
);
expectThrow(
  () => VectorIPC.decodeBase64("AA==AAAA"),
  "padding position",
  "mid-stream padding",
);
expectThrow(
  () => VectorIPC.decodeBase64("AB=="),
  "padding bits",
  "non-canonical two-padding bits",
);
expectThrow(
  () => VectorIPC.decodeBase64("AAB="),
  "padding bits",
  "non-canonical one-padding bits",
);

assert(VectorIPC.defaultChunkWords === 256, "default chunk words");
assert(VectorIPC.maxChunkWords === 256, "max chunk words");
assert(VectorIPC.version === 5, "wrapper version");
assert(VectorIPC.adapterVersion === 3, "adapter version");
assert(VectorIPC.supportsText === true, "text capability");
assert(VectorIPC.supportsESON === true, "ESON capability");
assert(VectorIPC.supportsESB64 === true, "ESB64 capability");
assert(VectorIPC.supportsESCHARS === true, "ESCHARS capability");

let adapterCalls = 0;
const fakeLib = {
  vipc() {
    adapterCalls++;
    throw new Error("adapter should not have been called");
  },
  unload() {},
};
const client = new VectorIPC.Client(fakeLib);
client.handle = 1;
expectThrow(
  () => client.stageByteArray([1], 257),
  "range 1..256",
  "chunk upper bound",
);
assert(adapterCalls === 0, "invalid chunk mutated adapter state");

const rollbackCommands = [];
let rollbackStageSize = 0;
const rollbackLib = {
  vipc(...args) {
    rollbackCommands.push(args[0]);
    if (args[0] === 2) {
      rollbackStageSize = 0;
      return "VIPC/1.0|OK|STAGE_RESET";
    }
    if (args[0] === 3) {
      rollbackStageSize += args[2];
      return "VIPC/1.0|OK|STAGED|" + rollbackStageSize;
    }
    throw new Error("unexpected rollback command " + args[0]);
  },
  unload() {},
};
const rollbackClient = new VectorIPC.Client(rollbackLib);
rollbackClient.handle = 2;
expectThrow(
  () => rollbackClient.stageByteArray([1, 2, 3, 4, 5, 6, 256], 1),
  "byte out of range at index 6",
  "invalid staged byte",
);
assert(
  rollbackCommands.join(",") === "2,3,2",
  "invalid byte rollback command sequence",
);
assert(rollbackStageSize === 0, "invalid byte left staged data");

rollbackCommands.length = 0;
rollbackStageSize = 99;
expectThrow(
  () => rollbackClient.stageBinaryString("A\u0100B", 256),
  "byte out of range at index 1",
  "invalid binary-string byte",
);
assert(
  rollbackCommands.join(",") === "2,2",
  "invalid binary-string rollback command sequence",
);
assert(rollbackStageSize === 0, "invalid binary string left staged data");

const packedCalls = [];
const packedLib = {
  vipc(...args) {
    packedCalls.push(args);
    if (args[0] === 2) return "VIPC/1.0|OK|STAGE_RESET";
    if (args[0] === 3) return "VIPC/1.0|OK|STAGED|" + args[2];
    throw new Error("unexpected adapter command " + args[0]);
  },
  unload() {},
};
const packedClient = new VectorIPC.Client(packedLib);
packedClient.handle = 17;
packedClient.stageByteArray([0, 1, 2, 255, 16, 128, 127], 1);
assert(packedCalls.length === 3, "packed path call count");
assert(
  packedCalls[0].length === 2
    && packedCalls[0][0] === 2
    && packedCalls[0][1] === 17,
  "packed reset call",
);
const packedFirst =
  0
  + 1 * 256
  + 2 * 65536
  + 255 * 16777216
  + 16 * 4294967296
  + 128 * 1099511627776;
assert(
  packedCalls[1].length === 4
    && packedCalls[1][0] === 3
    && packedCalls[1][1] === 17
    && packedCalls[1][2] === 6
    && packedCalls[1][3] === packedFirst,
  "packed first word",
);
assert(
  packedCalls[2].length === 4
    && packedCalls[2][0] === 3
    && packedCalls[2][1] === 17
    && packedCalls[2][2] === 1
    && packedCalls[2][3] === 127,
  "packed tail word",
);

const info = VectorIPC.parseResponse("VIPC/1.0|INFO|3|1|0|262144|6|16");
assert(info.ok && info.maxSessions === 16, "INFO parsing");

const frame = VectorIPC.parseResponse(
  "VIPC/1.0|FRAME|2|1|326|2882400001|305419896|4|AAEC/w==",
);
assert(
  frame.ok
    && frame.kind === 2
    && frame.flags === 1
    && frame.operation === 326
    && frame.payloadSize === 4,
  "FRAME parsing",
);

const error = VectorIPC.parseResponse("VIPC/1.0|ERR|7|8|121");
assert(
  !error.ok
    && error.status === 7
    && error.phase === 8
    && error.platformCode === 121,
  "ERR parsing",
);

const textFrame = VectorIPC.parseResponse(
  "VIPC/1.0|TEXT|2|0|327|123|456|15|héllo|x\n世界",
);
assert(
  textFrame.ok
    && textFrame.type === "text"
    && textFrame.kind === 2
    && textFrame.operation === 327
    && textFrame.correlationLow === 123
    && textFrame.correlationHigh === 456
    && textFrame.payloadSize === 15
    && textFrame.payloadText === "héllo|x\n世界",
  "TEXT parsing preserves delimiters and Unicode",
);

const textCalls = [];
const textLib = {
  vipc(...args) {
    textCalls.push(args);
    if (args[0] !== 8) throw new Error("unexpected text command " + args[0]);
    return [
      "VIPC/1.0",
      "TEXT",
      "2",
      "0",
      args[3],
      args[4],
      args[5],
      Buffer.byteLength(args[6], "utf8"),
      args[6],
    ].join("|");
  },
  unload() {},
};
const textClient = new VectorIPC.Client(textLib);
textClient.handle = 257;
const textPayload = "héllo|x\n世界";
const textResponse = textClient.requestText(
  327,
  textPayload,
  2000,
  { low: 123, high: 456 },
);
assert(textResponse.payloadText === textPayload, "requestText payload");
assert(textResponse.payloadSize === 15, "requestText UTF-8 byte size");
assert(textCalls.length === 1 && textCalls[0][0] === 8, "requestText command");

const fakeESON = {
  stringify(value) {
    return JSON.stringify(value);
  },
  parse(text) {
    return JSON.parse(text);
  },
};
const esonInput = { a: 1, b: [true, null, "x"] };
const esonResponse = textClient.requestESON(
  327,
  esonInput,
  2000,
  { low: 124, high: 456 },
  fakeESON,
);
assert(esonResponse.value.a === 1, "requestESON object field");
assert(esonResponse.value.b[0] === true, "requestESON array field");
assert(esonResponse.value.b[2] === "x", "requestESON string field");
assert(textCalls[textCalls.length - 1][0] === 8, "requestESON uses text command");

const jsonAliasResponse = textClient.requestJSON(
  327,
  { alias: "ok" },
  2000,
  { low: 125, high: 456 },
  fakeESON,
);
assert(jsonAliasResponse.value.alias === "ok", "requestJSON alias");

expectThrow(
  () => textClient.requestESON(
    327,
    { a: 1 },
    2000,
    { low: 126, high: 456 },
    {},
  ),
  "requires ESON",
  "requestESON missing codec",
);

expectThrow(
  () => textClient.requestESON(
    327,
    { a: 1 },
    2000,
    { low: 127, high: 456 },
    {
      stringify() {
        return undefined;
      },
      parse() {
        return null;
      },
    },
  ),
  "serialized to undefined",
  "requestESON undefined serialization",
);

const fakeESB64 = {
  atob(value) {
    return Buffer.from(value, "base64").toString("latin1");
  },
};
const binaryString = VectorIPC.decodeBase64BinaryString(
  "AAEC/xCAfw==",
  fakeESB64,
);
assert(binaryString.length === 7, "ESB64 decoded length");
assert(binaryString.charCodeAt(0) === 0, "ESB64 preserves NUL");
assert(binaryString.charCodeAt(3) === 255, "ESB64 preserves high byte");

const fakeESCHARS = {
  b64ToHex(value) {
    return Buffer.from(value, "base64").toString("hex");
  },
};
assert(
  VectorIPC.decodeBase64Hex("AAEC/xCAfw==", fakeESCHARS)
    === "000102ff10807f",
  "ESCHARS hex decode",
);

expectThrow(
  () => VectorIPC.decodeBase64BinaryString("Zg==", {}),
  "requires ESB64",
  "missing ESB64 integration",
);
expectThrow(
  () => VectorIPC.decodeBase64Hex("Zg==", {}),
  "requires ESCHARS",
  "missing ESCHARS integration",
);

const textCallsBeforeInvalid = textCalls.length;
expectThrow(
  () => textClient.requestText(327, "a\u0000b", 2000, { low: 1, high: 0 }),
  "cannot contain NUL",
  "requestText NUL rejection",
);
assert(
  textCalls.length === textCallsBeforeInvalid,
  "NUL text reached adapter",
);

expectThrow(
  () => textClient.requestText(327, "\uD800", 2000, { low: 2, high: 0 }),
  "unpaired UTF-16 surrogate",
  "lone high surrogate rejection",
);
expectThrow(
  () => textClient.requestText(327, "\uDC00", 2000, { low: 3, high: 0 }),
  "unpaired UTF-16 surrogate",
  "lone low surrogate rejection",
);
assert(
  textCalls.length === textCallsBeforeInvalid,
  "invalid surrogate text reached adapter",
);

expectThrow(
  () => VectorIPC.parseResponse("VIPC/1.0|TEXT|2|0|327|1|0"),
  "TEXT response",
  "truncated TEXT response",
);

expectThrow(
  () => VectorIPC.parseResponse("VIPC/1.0|INFO|3|1|0|Infinity|6|16"),
  "numeric field",
  "infinite numeric field rejection",
);
expectThrow(
  () => VectorIPC.parseResponse("VIPC/1.0|FRAME|2|0|326|1.5|0|4|AAEC/w=="),
  "numeric field",
  "fractional numeric field rejection",
);
expectThrow(
  () => VectorIPC.parseResponse("VIPC/1.0|FRAME|2|0|326|4294967296|0|4|AAEC/w=="),
  "numeric field",
  "uint32 overflow rejection",
);

console.log("VectorIPC ES3 wrapper unit tests: PASS");
