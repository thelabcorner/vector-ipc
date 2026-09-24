import { execFileSync } from "node:child_process";
import { resolve } from "node:path";

const root = resolve(import.meta.dirname, "..");
const config = process.env.VIPC_CONFIG || "Release";
const exeDir = process.platform === "win32"
  ? resolve(root, "build", config)
  : resolve(root, "build");
const ext = process.platform === "win32" ? ".exe" : "";

function run(name, args = []) {
  const exe = resolve(exeDir, name + ext);
  console.log("> " + exe + " " + args.join(" "));
  execFileSync(exe, args, { cwd: root, stdio: "inherit" });
}

execFileSync(process.execPath, ["tools/build.mjs", "build"], {
  cwd: root,
  stdio: "inherit"
});

run("vectoripc_transport_boundaries");
run("vectoripc_timeout_stress", ["2000"]);
run("vectoripc_cancellation_stress", ["2000", "500"]);
run("vectoripc_full_duplex_stress", ["100000"]);
run("vectoripc_transport_soak", ["1000000", "4096"]);
run("vectoripc_transport_soak", ["50000", "262144"]);
