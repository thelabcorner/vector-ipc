"""Run repeatable live Illustrator ExternalObject latency benchmarks."""

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


def _parse_fields(result: str) -> dict[str, str]:
    parts = result.split("|")
    if not parts or parts[0] != "PASS":
        return {}
    fields: dict[str, str] = {}
    for item in parts[1:]:
        if "=" in item:
            key, value = item.split("=", 1)
            fields[key] = value
    return fields


def main() -> int:
    repo = Path(__file__).resolve().parent.parent
    scripts_root = repo.parent
    com_skill = scripts_root / "agent-skills" / "illustrator-com-automation-skill"
    tool = com_skill / "comtool" / "ILLUSTRATOR_COM_TOOL.py"
    probe = repo / "tests" / "illustrator_externalobject_bench.jsx"
    source_dll = repo / "build" / "Release" / "VectorIPCExternalObject.dll"
    echo_server = repo / "build" / "Release" / "vectoripc_ping_bench.exe"

    for required in (tool, probe, source_dll, echo_server):
        if not required.is_file():
            raise FileNotFoundError(required)

    runs: list[dict[str, object]] = []
    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)

    for run_index in range(3):
        live_dir, probe_name, live_dll = stage_live_dll(
            source_dll,
            f"VectorIPCExternalObjectBench_{run_index:02x}",
        )
        endpoint = f"eob{(os.getpid() + run_index) % 1_000_000_000:09d}"

        server = subprocess.Popen(
            [str(echo_server), "--server", endpoint, "2200"],
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
                    json.dumps([live_dir.as_posix(), probe_name, endpoint]),
                    "--timeout",
                    "30",
                ],
                cwd=com_skill,
                capture_output=True,
                text=True,
                check=False,
                timeout=40,
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
            fields = _parse_fields(result) if isinstance(result, str) else {}
            ok = (
                completed.returncode == 0
                and envelope.get("ok") is True
                and isinstance(result, str)
                and result.startswith("PASS|adapter=3|")
                and server.returncode == 0
            )

            released = try_remove_live_dll(live_dll)

            record: dict[str, object] = {
                "run": run_index + 1,
                "ok": ok,
                "result": result,
                "fields": fields,
                "serverExit": server.returncode,
                "dllReleasedAfterUnload": released,
                "dll": str(live_dll),
            }
            if not ok:
                record.update(
                    {
                        "comStdout": completed.stdout,
                        "comStderr": completed.stderr,
                        "serverStdout": server_stdout,
                        "serverStderr": server_stderr,
                    }
                )
            runs.append(record)

            if not ok:
                print(json.dumps({"ok": False, "runs": runs}, ensure_ascii=False))
                return 1
        finally:
            if server.poll() is None:
                server.kill()
                try:
                    server.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    pass

    medians = [float(r["fields"]["median_us"]) for r in runs]  # type: ignore[index]
    p95s = [float(r["fields"]["p95_us"]) for r in runs]  # type: ignore[index]
    medians_sorted = sorted(medians)
    p95s_sorted = sorted(p95s)

    report = {
        "ok": True,
        "adapterVersion": 3,
        "runs": runs,
        "medianOfRunMediansUs": medians_sorted[len(medians_sorted) // 2],
        "medianOfRunP95Us": p95s_sorted[len(p95s_sorted) // 2],
    }
    print(json.dumps(report, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
