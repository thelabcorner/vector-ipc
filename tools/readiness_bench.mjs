import { execFileSync } from "node:child_process";
import { resolve } from "node:path";

const root = resolve(import.meta.dirname, "..");
const config = process.env.VIPC_CONFIG || "Release";
const args = process.argv.slice(2);

if (args.length > 0 && args[0].startsWith("-")) {
  console.log(
    "usage: npm run bench:idle -- [idle-interval-ms] [idle-samples] " +
      "[wake-spread-ms] [wake-samples] [cpu-budget-pct]"
  );
  console.log(
    "  idle-interval-ms  length of one idle readiness wait (default 15000)"
  );
  console.log("  idle-samples      number of idle waits (default 2)");
  console.log("  wake-spread-ms    window the peer send offset is spread over");
  console.log("  wake-samples      number of readiness wake samples");
  console.log("  cpu-budget-pct    fail above this share of one core (default 2.0)");
  console.log(
    "example: npm run bench:idle -- 60000 5 200 60   # 300 s of idle waiting"
  );
  process.exit(0);
}

execFileSync(process.execPath, ["tools/build.mjs", "build"], {
  cwd: root,
  stdio: "inherit"
});

const exe = process.platform === "win32"
  ? resolve(root, "build", config, "vectoripc_readiness_idle_bench.exe")
  : resolve(root, "build", "vectoripc_readiness_idle_bench");

execFileSync(exe, args, { cwd: root, stdio: "inherit" });
