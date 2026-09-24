"""Live Illustrator certification of the canonical VectorIPC ES3 wrapper."""

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
    probe = repo / "tests" / "illustrator_wrapper_probe.jsx"
    wrapper = repo / "src" / "adapters" / "externalobject" / "VectorIPC.jsx"
    source_dll = repo / "build" / "Release" / "VectorIPCExternalObject.dll"
    echo_server = repo / "build" / "Release" / "vectoripc_ping_bench.exe"

    for required in (tool, probe, wrapper, source_dll, echo_server):
        if not required.is_file():
            raise FileNotFoundError(required)

    live_dir, probe_name, live_dll = stage_live_dll(
        source_dll,
        "VectorIPCExternalObjectProbe",
    )

    endpoint = f"eow{os.getpid() % 1_000_000_000:09d}"
    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    server = subprocess.Popen(
        [str(echo_server), "--server", endpoint, "2"],
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
                json.dumps([
                    live_dir.as_posix(),
                    wrapper.as_posix(),
                    probe_name,
                    endpoint,
                ]),
                "--timeout",
                "15",
            ],
            cwd=com_skill,
            capture_output=True,
            text=True,
            check=False,
            timeout=25,
        )

        try:
            server_stdout, server_stderr = server.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server_stdout, server_stderr = server.communicate(timeout=2)

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
            and result.startswith("PASS|1024|handle=")
            and server.returncode == 0
        )

        released = try_remove_live_dll(live_dll)

        report = {
            "ok": ok,
            "result": result,
            "comExit": completed.returncode,
            "serverExit": server.returncode,
            "dllReleasedAfterUnload": released,
            "dll": str(live_dll),
        }
        if not ok:
            report.update(
                {
                    "comStdout": completed.stdout,
                    "comStderr": completed.stderr,
                    "serverStdout": server_stdout,
                    "serverStderr": server_stderr,
                }
            )

        print(json.dumps(report, ensure_ascii=False))
        return 0 if ok else 1
    finally:
        if server.poll() is None:
            server.kill()
            try:
                server.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass


if __name__ == "__main__":
    raise SystemExit(main())
