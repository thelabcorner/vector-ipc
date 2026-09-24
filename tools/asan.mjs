import { execFileSync } from "node:child_process";
import { existsSync, readdirSync, rmSync } from "node:fs";
import { join, resolve } from "node:path";

const root = resolve(import.meta.dirname, "..");
const buildDir = resolve(root, "build-asan");
const vswhere =
  "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe";

function run(file, args, options = {}) {
  console.log("> " + file + " " + args.join(" "));
  execFileSync(file, args, {
    cwd: root,
    stdio: "inherit",
    ...options,
  });
}

if (process.platform !== "win32") {
  throw new Error("VectorIPC MSVC AddressSanitizer gate currently requires Windows");
}
if (!existsSync(vswhere)) {
  throw new Error("vswhere.exe not found: " + vswhere);
}

const vsInstall = execFileSync(
  vswhere,
  [
    "-latest",
    "-products",
    "*",
    "-requires",
    "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
    "-property",
    "installationPath",
  ],
  { encoding: "utf8" },
).trim();

if (!vsInstall) {
  throw new Error("Visual Studio C++ Build Tools were not found");
}

const msvcRoot = join(vsInstall, "VC", "Tools", "MSVC");
const versions = readdirSync(msvcRoot, { withFileTypes: true })
  .filter((entry) => entry.isDirectory())
  .map((entry) => entry.name)
  .sort((a, b) => a.localeCompare(b, undefined, { numeric: true }));

if (versions.length === 0) {
  throw new Error("No MSVC toolsets found under " + msvcRoot);
}

const toolset = versions[versions.length - 1];
const runtimeDir = join(msvcRoot, toolset, "bin", "Hostx64", "x64");
const asanRuntime = join(runtimeDir, "clang_rt.asan_dynamic-x86_64.dll");

if (!existsSync(asanRuntime)) {
  throw new Error("MSVC ASan runtime not found: " + asanRuntime);
}

rmSync(buildDir, { recursive: true, force: true });

run("cmake", [
  "-S", ".",
  "-B", "build-asan",
  "-G", "Visual Studio 17 2022",
  "-A", "x64",
  "-DVIPC_BUILD_TESTS=ON",
  "-DVIPC_BUILD_BENCHMARKS=OFF",
  "-DCMAKE_C_FLAGS=/fsanitize=address /Zi",
  "-DCMAKE_CXX_FLAGS=/fsanitize=address /Zi",
  "-DCMAKE_EXE_LINKER_FLAGS=/DEBUG",
  "-DCMAKE_SHARED_LINKER_FLAGS=/DEBUG",
]);

run("cmake", [
  "--build", "build-asan",
  "--config", "Release",
  "--parallel",
]);

const env = {
  ...process.env,
  PATH: runtimeDir + ";" + (process.env.PATH || ""),
};

run(
  "ctest",
  [
    "--test-dir", "build-asan",
    "-C", "Release",
    "--output-on-failure",
  ],
  { env },
);

console.log(
  "VectorIPC MSVC AddressSanitizer gate: PASS (" +
    toolset +
    ", runtime=" +
    asanRuntime +
    ")",
);
