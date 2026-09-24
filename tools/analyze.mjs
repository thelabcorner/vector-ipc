import { execFileSync } from "node:child_process";
import { rmSync } from "node:fs";
import { resolve } from "node:path";

const root = resolve(import.meta.dirname, "..");
const buildDir = resolve(root, "build-analyze");

function run(file, args) {
  console.log("> " + file + " " + args.join(" "));
  execFileSync(file, args, { cwd: root, stdio: "inherit" });
}

if (process.platform !== "win32") {
  throw new Error("VectorIPC MSVC static-analysis gate currently requires Windows");
}

rmSync(buildDir, { recursive: true, force: true });

run("cmake", [
  "-S", ".",
  "-B", "build-analyze",
  "-G", "Visual Studio 17 2022",
  "-A", "x64",
  "-DVIPC_BUILD_TESTS=ON",
  "-DVIPC_BUILD_BENCHMARKS=OFF",
  "-DCMAKE_C_FLAGS=/analyze",
  "-DCMAKE_CXX_FLAGS=/analyze",
]);

run("cmake", [
  "--build", "build-analyze",
  "--config", "Release",
  "--parallel",
]);

console.log("VectorIPC MSVC static-analysis gate: PASS");
