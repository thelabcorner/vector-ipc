import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import {
  mkdirSync,
  readFileSync,
  statSync,
  writeFileSync,
} from "node:fs";
import { basename, dirname, resolve } from "node:path";

const root = resolve(import.meta.dirname, "..");

function fail(message) {
  throw new Error(message);
}

function gitText(args) {
  return execFileSync("git", args, {
    cwd: root,
    encoding: "utf8",
    maxBuffer: 16 * 1024 * 1024,
  }).trim();
}

function gitBytes(args) {
  return execFileSync("git", args, {
    cwd: root,
    encoding: null,
    maxBuffer: 64 * 1024 * 1024,
  });
}

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

function parseArgs(argv) {
  const result = {
    allowUntagged: false,
    output: null,
    noArchive: false,
  };
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === "--allow-untagged") result.allowUntagged = true;
    else if (arg === "--no-archive") result.noArchive = true;
    else if (arg === "--output") {
      i += 1;
      if (i >= argv.length) fail("--output requires a path");
      result.output = argv[i];
    } else {
      fail("unknown argument: " + arg);
    }
  }
  return result;
}

function macro(text, name) {
  const re = new RegExp(
    "^\\s*#define\\s+" + name + "\\s+(?:\\\"([^\\\"]+)\\\"|([0-9]+)u?)\\s*$",
    "m",
  );
  const match = text.match(re);
  if (!match) fail("missing macro " + name);
  return match[1] ?? Number(match[2]);
}

function fileRecords(paths) {
  return paths.map((path) => {
    const bytes = gitBytes(["show", "HEAD:" + path]);
    return {
      path,
      bytes: bytes.length,
      sha256: sha256(bytes),
    };
  });
}

function aggregate(records) {
  const hash = createHash("sha256");
  for (const record of records) {
    hash.update(record.path, "utf8");
    hash.update(Buffer.from([0]));
    hash.update(record.sha256, "ascii");
    hash.update("\n", "ascii");
  }
  return hash.digest("hex");
}

const args = parseArgs(process.argv.slice(2));
const packageJson = JSON.parse(
  readFileSync(resolve(root, "package.json"), "utf8"),
);
const cmake = readFileSync(resolve(root, "CMakeLists.txt"), "utf8");
const publicHeader = readFileSync(
  resolve(root, "include/vectoripc/vipc.h"),
  "utf8",
);
const protocolHeader = readFileSync(
  resolve(root, "include/vectoripc/vipc_protocol.h"),
  "utf8",
);
const wrapper = readFileSync(
  resolve(root, "src/adapters/externalobject/VectorIPC.jsx"),
  "utf8",
);
const adapter = readFileSync(
  resolve(root, "src/adapters/externalobject/vipc_externalobject.c"),
  "utf8",
);

const version = packageJson.version;
const cmakeVersion = cmake.match(
  /project\(VectorIPC\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)\s+LANGUAGES/,
)?.[1];
if (!cmakeVersion) fail("could not parse VectorIPC CMake version");
if (cmakeVersion !== version) {
  fail(`version mismatch: package.json=${version}, CMake=${cmakeVersion}`);
}

const headerVersion = [
  macro(publicHeader, "VIPC_VERSION_MAJOR"),
  macro(publicHeader, "VIPC_VERSION_MINOR"),
  macro(publicHeader, "VIPC_VERSION_PATCH"),
].join(".");
if (headerVersion !== version) {
  fail(`version mismatch: package.json=${version}, headers=${headerVersion}`);
}
if (macro(publicHeader, "VIPC_VERSION_STRING") !== version) {
  fail("VIPC_VERSION_STRING does not match package.json");
}

const abiVersion = macro(publicHeader, "VIPC_ABI_VERSION");
const protocolVersion = macro(
  protocolHeader,
  "VIPC_PROTOCOL_VERSION_STRING",
);
const wireMajor = macro(protocolHeader, "VIPC_WIRE_MAJOR");
const wireMinor = macro(protocolHeader, "VIPC_WIRE_MINOR");
const esabiVersion = cmake.match(
  /set\(VIPC_ESABI_VERSION\s+"([^"]+)"\)/,
)?.[1];
const esabiCommit = cmake.match(
  /set\(VIPC_ESABI_GIT_COMMIT\s+"([0-9a-f]{40})"\)/,
)?.[1];
if (!esabiVersion) fail("could not parse ESABI dependency version");
if (!esabiCommit) fail("could not parse ESABI dependency commit");
const adapterVersion = Number(
  adapter.match(/#define\s+VIPC_EO_ADAPTER_VERSION\s+([0-9]+)u/)?.[1],
);
const wrapperAdapterVersion = Number(
  wrapper.match(/var\s+ADAPTER_VERSION\s*=\s*([0-9]+);/)?.[1],
);
const wrapperVersion = Number(
  wrapper.match(/\n\s*version:\s*([0-9]+),/)?.[1],
);
if (!Number.isInteger(adapterVersion)) fail("could not parse adapter version");
if (!Number.isInteger(wrapperAdapterVersion)) {
  fail("could not parse wrapper adapter version");
}
if (wrapperAdapterVersion !== adapterVersion) {
  fail(
    `adapter mismatch: native=${adapterVersion}, wrapper=${wrapperAdapterVersion}`,
  );
}
if (!Number.isInteger(wrapperVersion)) fail("could not parse wrapper version");

const status = gitText(["status", "--porcelain=v1", "--untracked-files=all"]);
if (status) {
  fail("working tree is dirty; release manifests require an immutable commit");
}

const commit = gitText(["rev-parse", "HEAD"]);
const expectedTag = "v" + version;
const tagsAtHead = gitText(["tag", "--points-at", "HEAD"])
  .split(/\r?\n/)
  .filter(Boolean);
if (!tagsAtHead.includes(expectedTag) && !args.allowUntagged) {
  fail(`HEAD is not tagged ${expectedTag}`);
}

const tracked = gitText(["ls-files"])
  .split(/\r?\n/)
  .filter(Boolean)
  .sort();

const headerPaths = tracked.filter((path) =>
  path.startsWith("include/vectoripc/"),
);
const sourcePaths = tracked.filter(
  (path) =>
    path === "CMakeLists.txt" ||
    path.startsWith("cmake/") ||
    path.startsWith("include/vectoripc/") ||
    path.startsWith("src/"),
);
if (headerPaths.length === 0 || sourcePaths.length === 0) {
  fail("release digest file sets are empty");
}

const headerRecords = fileRecords(headerPaths);
const sourceRecords = fileRecords(sourcePaths);

const releaseDir = resolve(root, "build", "release");
mkdirSync(releaseDir, { recursive: true });

let archive = null;
if (!args.noArchive) {
  const archivePath = resolve(
    releaseDir,
    `vector-ipc-v${version}-source.zip`,
  );
  execFileSync(
    "git",
    [
      "archive",
      "--format=zip",
      `--prefix=vector-ipc-${version}/`,
      `--output=${archivePath}`,
      "HEAD",
    ],
    { cwd: root, stdio: "inherit" },
  );
  const bytes = readFileSync(archivePath);
  archive = {
    file: basename(archivePath),
    bytes: statSync(archivePath).size,
    sha256: sha256(bytes),
  };
}

const manifest = {
  schema: "vectoripc.release-lock/v1",
  project: "VectorIPC",
  version,
  tag: tagsAtHead.includes(expectedTag) ? expectedTag : null,
  commit,
  clean: !status,
  compatibility: {
    abiVersion,
    protocol: protocolVersion,
    wireMajor,
    wireMinor,
    externalObjectAdapterVersion: adapterVersion,
    extendScriptWrapperVersion: wrapperVersion,
  },
  dependencies: {
    esabi: {
      version: esabiVersion,
      tag: "v" + esabiVersion,
      commit: esabiCommit,
      repository: "https://github.com/thelabcorner/esabi.git",
    },
  },
  digests: {
    algorithm: "sha256",
    aggregateAlgorithm:
      "sha256(sorted UTF-8 path + NUL + lowercase file sha256 + LF)",
    publicHeaders: {
      sha256: aggregate(headerRecords),
      files: headerRecords,
    },
    source: {
      sha256: aggregate(sourceRecords),
      files: sourceRecords,
    },
  },
  archive,
};

const output = resolve(
  root,
  args.output ??
    `build/release/vector-ipc-v${version}.lock.json`,
);
mkdirSync(dirname(output), { recursive: true });
writeFileSync(output, JSON.stringify(manifest, null, 2) + "\n", "utf8");

console.log("VectorIPC release manifest: PASS");
console.log("  version: " + version);
console.log("  tag: " + (manifest.tag ?? "<untagged>"));
console.log("  commit: " + commit);
console.log("  public headers: " + manifest.digests.publicHeaders.sha256);
console.log("  source: " + manifest.digests.source.sha256);
if (archive) console.log("  archive: " + archive.sha256);
console.log("  output: " + output);
