"""Live Illustrator certification of ExternalObject logical-session isolation."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys

from live_support import stage_live_dll, try_remove_live_dll


def _raw_result(envelope: dict) -> object:
    value = envelope.get("result")
    if isinstance(value, dict) and "result" in value:
        return value["result"]
    return value


def main() -> int:
    repo = Path(__file__).resolve().parent.parent
    scripts_root = repo.parent
    com_skill = scripts_root / "agent-skills" / "illustrator-com-automation-skill"
    tool = com_skill / "comtool" / "ILLUSTRATOR_COM_TOOL.py"
    probe = repo / "tests" / "illustrator_multisession_probe.jsx"
    wrapper = repo / "src" / "adapters" / "externalobject" / "VectorIPC.jsx"
    source_dll = repo / "build" / "Release" / "VectorIPCExternalObject.dll"
    echo_server = repo / "build" / "Release" / "vectoripc_ping_bench.exe"

    for required in (tool, probe, wrapper, source_dll, echo_server):
        if not required.is_file():
            raise FileNotFoundError(required)

    live_dir, probe_name, live_dll = stage_live_dll(
        source_dll,
        "VectorIPCExternalObjectSessions",
    )

    endpoint_a = f"eoa{os.getpid() % 1_000_000_000:09d}"
    endpoint_b = f"eob{(os.getpid() + 1) % 1_000_000_000:09d}"
    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)

    server_a = subprocess.Popen(
        [str(echo_server), "--server", endpoint_a, "1"],
        cwd=repo,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        creationflags=creationflags,
    )
    server_b = subprocess.Popen(
        [str(echo_server), "--server", endpoint_b, "2"],
        cwd=repo,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        creationflags=creationflags,
    )

    try:
        completed = subprocess.run(
            [
                sys.executable,
                str(tool),
                "eval",
                "--file",
                str(probe),
                "--args-json",
                json.dumps(
                    [
                        live_dir.as_posix(),
                        wrapper.as_posix(),
                        probe_name,
                        endpoint_a,
                        endpoint_b,
                    ]
                ),
                "--timeout",
                "15",
            ],
            cwd=com_skill,
            capture_output=True,
            text=True,
            check=False,
            timeout=25,
        )

        outputs: list[tuple[str, subprocess.Popen[str]]] = [
            ("A", server_a),
            ("B", server_b),
        ]
        server_details: dict[str, dict[str, object]] = {}
        all_servers_ok = True
        for label, server in outputs:
            try:
                stdout, stderr = server.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                stdout, stderr = server.communicate(timeout=2)
            server_details[label] = {
                "exit": server.returncode,
                "stdout": stdout,
                "stderr": stderr,
            }
            if server.returncode != 0:
                all_servers_ok = False

        lines = [
            line.strip()
            for line in (completed.stdout or "").splitlines()
            if line.strip()
        ]
        envelope: dict = {}
        if lines:
            try:
                envelope = json.loads(lines[-1])
            except json.JSONDecodeError:
                pass

        result = _raw_result(envelope)
        ok = (
            completed.returncode == 0
            and envelope.get("ok") is True
            and isinstance(result, str)
            and result.startswith("PASS|closedA=")
            and "|liveB=" in result
            and result.endswith("|payloadB=ALGygH8=")
            and all_servers_ok
        )

        released = try_remove_live_dll(live_dll)

        report: dict[str, object] = {
            "ok": ok,
            "result": result,
            "comExit": completed.returncode,
            "servers": {
                key: value["exit"] for key, value in server_details.items()
            },
            "dllReleasedAfterUnload": released,
            "dll": str(live_dll),
        }
        if not ok:
            report["comStdout"] = completed.stdout
            report["comStderr"] = completed.stderr
            report["serverDetails"] = server_details

        print(json.dumps(report, ensure_ascii=False))
        return 0 if ok else 1
    finally:
        for server in (server_a, server_b):
            if server.poll() is None:
                server.kill()
                try:
                    server.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    pass


if __name__ == "__main__":
    raise SystemExit(main())
