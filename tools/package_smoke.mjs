import { execFileSync } from "node:child_process";
import {
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

const root = resolve(import.meta.dirname, "..");
const config = process.env.VIPC_CONFIG || "Release";
const smokeParent = process.env.LOCALAPPDATA
  ? resolve(process.env.LOCALAPPDATA, "VectorIPC", "package-smoke")
  : resolve(tmpdir(), "VectorIPC-package-smoke");
mkdirSync(smokeParent, { recursive: true });
const smokeRoot = mkdtempSync(join(smokeParent, "run-"));
const installDir = resolve(smokeRoot, "install");
const consumerBuild = resolve(smokeRoot, "consumer");
const incompatibleSource = resolve(smokeRoot, "incompatible-source");
const incompatibleBuild = resolve(smokeRoot, "incompatible");

function run(file, args, cwd = root) {
  console.log("> " + file + " " + args.join(" "));
  execFileSync(file, args, { cwd, stdio: "inherit" });
}

function cleanup() {
  try {
    rmSync(smokeRoot, {
      recursive: true,
      force: true,
      maxRetries: 10,
      retryDelay: 100,
    });
  } catch (error) {
    // Smoke correctness has already been established at this point. Windows
    // toolchains may briefly retain build-tree handles after process exit; the
    // OS temp directory is the correct place for any delayed cleanup residue.
    console.warn(
      "VectorIPC package smoke cleanup deferred: " +
        (error instanceof Error ? error.message : String(error)),
    );
  }
}

try {
  run("cmake", [
    "--install", "build",
    "--config", config,
    "--prefix", installDir
  ]);

  const installedLicense = resolve(
    installDir,
    "share",
    "licenses",
    "VectorIPC",
    "LICENSE",
  );
  const licenseText = readFileSync(installedLicense, "utf8");
  if (!licenseText.startsWith("MIT License")) {
    throw new Error("installed package is missing the expected MIT LICENSE");
  }

  const configureArgs = [
    "-S", "tests/package_consumer",
    "-B", consumerBuild,
    "-DCMAKE_PREFIX_PATH=" + installDir
  ];
  if (process.platform === "win32") {
    configureArgs.push("-G", "Visual Studio 17 2022", "-A", "x64");
  }
  run("cmake", configureArgs);
  run("cmake", ["--build", consumerBuild, "--config", config, "--parallel"]);

  const executableC = process.platform === "win32"
    ? resolve(consumerBuild, config, "vectoripc_package_consumer_c.exe")
    : resolve(consumerBuild, "vectoripc_package_consumer_c");
  const executableCpp = process.platform === "win32"
    ? resolve(consumerBuild, config, "vectoripc_package_consumer.exe")
    : resolve(consumerBuild, "vectoripc_package_consumer");
  run(executableC, []);
  run(executableCpp, []);

  mkdirSync(incompatibleSource, { recursive: true });
  writeFileSync(
    resolve(incompatibleSource, "CMakeLists.txt"),
    [
      "cmake_minimum_required(VERSION 3.20)",
      "project(VectorIPCIncompatibleVersionCheck NONE)",
      "find_package(VectorIPC 0.2 CONFIG QUIET)",
      "if(VectorIPC_FOUND)",
      "  message(FATAL_ERROR \"VectorIPC 0.1.x incorrectly satisfies 0.2\")",
      "endif()",
      "message(STATUS \"VectorIPC 0.2 correctly rejected by 0.1.x package\")",
      "",
    ].join("\n"),
    "utf8",
  );

  run("cmake", [
    "-S", incompatibleSource,
    "-B", incompatibleBuild,
    "-DCMAKE_PREFIX_PATH=" + installDir,
  ]);

  console.log("VectorIPC installed-package smoke: PASS");
} finally {
  cleanup();
}
