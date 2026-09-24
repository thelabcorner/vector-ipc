import { execFileSync } from "node:child_process";
import { resolve } from "node:path";

const root = resolve(import.meta.dirname, "..");
const config = process.env.VIPC_CONFIG || "Release";

execFileSync(process.execPath, ["tools/build.mjs", "build"], {
  cwd: root,
  stdio: "inherit"
});

const exe = process.platform === "win32"
  ? resolve(root, "build", config, "vectoripc_matrix_bench.exe")
  : resolve(root, "build", "vectoripc_matrix_bench");

execFileSync(exe, [], { cwd: root, stdio: "inherit" });
