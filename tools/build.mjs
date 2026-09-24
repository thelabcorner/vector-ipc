import { execFileSync } from "node:child_process";
import { existsSync, readFileSync, rmSync } from "node:fs";
import { resolve } from "node:path";

const root = resolve(import.meta.dirname, "..");
const buildDir = resolve(root, "build");
const mode = process.argv[2] || "test";
const config = process.env.VIPC_CONFIG || "Release";

function run(file, args) {
  console.log("> " + file + " " + args.join(" "));
  execFileSync(file, args, { cwd: root, stdio: "inherit" });
}

function assertWrapperMirrors() {
  const jsx = readFileSync(
    resolve(root, "src", "adapters", "externalobject", "VectorIPC.jsx")
  );
  const jsxinc = readFileSync(
    resolve(root, "src", "adapters", "externalobject", "VectorIPC.jsxinc")
  );
  if (!jsx.equals(jsxinc)) {
    throw new Error(
      "VectorIPC.jsx and VectorIPC.jsxinc diverged; keep distribution mirrors identical"
    );
  }
}

if (mode === "clean") {
  rmSync(buildDir, { recursive: true, force: true });
  process.exit(0);
}

assertWrapperMirrors();

if (!existsSync(resolve(buildDir, "CMakeCache.txt"))) {
  if (process.platform === "win32") {
    run("cmake", [
      "-S", ".",
      "-B", "build",
      "-G", "Visual Studio 17 2022",
      "-A", "x64",
      "-DVIPC_BUILD_TESTS=ON"
    ]);
  } else {
    run("cmake", ["-S", ".", "-B", "build", "-DVIPC_BUILD_TESTS=ON"]);
  }
}

run("cmake", ["--build", "build", "--config", config, "--parallel"]);

if (mode === "test") {
  run("ctest", [
    "--test-dir", "build",
    "-C", config,
    "--output-on-failure"
  ]);
}