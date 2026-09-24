"""Benchmark the canonical VectorIPC ES3 wrapper inside live Illustrator."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from live_support import stage_live_dll, try_remove_live_dll


QUICK_SERVER_ITERATIONS = 25
FULL_SERVER_ITERATIONS = 167


def _raw_result(envelope: dict) -> object:
    value = envelope.get("result")
    if isinstance(value, dict) and "result" in value:
        return value["result"]
    return value


def _parse_result(result: str) -> dict[str, object]:
    parts = result.split("|")
    if len(parts) < 3 or parts[0] != "PASS":
        return {}
    records: list[dict[str, object]] = []
    for item in parts[3:]:
        if ":" not in item:
            continue
        kind, raw_fields = item.split(":", 1)
        record: dict[str, object] = {"kind": kind}
        for field in raw_fields.split(","):
            key, value = field.split("=", 1)
            if key in {"mean"}:
                record[key] = float(value)
            else:
                record[key] = int(float(value))
        records.append(record)
    return {"records": records}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--full",
        action="store_true",
        help="run the exhaustive ES3 workload (can block Illustrator for minutes)",
    )
    args = parser.parse_args()
    mode = "full" if args.full else "quick"
    expected_server_iterations = (
        FULL_SERVER_ITERATIONS if args.full else QUICK_SERVER_ITERATIONS
    )
    expected_records = 11 if args.full else 5

    repo = Path(__file__).resolve().parent.parent
    scripts_root = repo.parent
    com_skill = scripts_root / "agent-skills" / "illustrator-com-automation-skill"
    tool = com_skill / "comtool" / "ILLUSTRATOR_COM_TOOL.py"
    probe = repo / "tests" / "illustrator_wrapper_bench.jsx"
    wrapper = repo / "src" / "adapters" / "externalobject" / "VectorIPC.jsx"
    source_dll = repo / "build" / "Release" / "VectorIPCExternalObject.dll"
    echo_server = repo / "build" / "Release" / "vectoripc_matrix_bench.exe"

    for required in (tool, probe, wrapper, source_dll, echo_server):
        if not required.is_file():
            raise FileNotFoundError(required)

    live_dir, probe_name, live_dll = stage_live_dll(
        source_dll,
        "VectorIPCWrapperBench",
    )
    endpoint = f"eowb{os.getpid() % 100_000_000:08d}"

    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    server = subprocess.Popen(
        [
            str(echo_server),
            "--server",
            endpoint,
            str(expected_server_iterations),
        ],
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
                    mode,
                ]),
                "--timeout",
                "180" if args.full else "30",
            ],
            cwd=com_skill,
            capture_output=True,
            text=True,
            check=False,
            timeout=200 if args.full else 45,
        )

        try:
            server_stdout, server_stderr = server.communicate(timeout=10)
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
        parsed = _parse_result(result) if isinstance(result, str) else {}
        ok = (
            completed.returncode == 0
            and envelope.get("ok") is True
            and isinstance(result, str)
            and result.startswith("PASS|wrapper=5|adapter=3|")
            and server.returncode == 0
            and len(parsed.get("records", [])) == expected_records
        )

        released = try_remove_live_dll(live_dll)

        report: dict[str, object] = {
            "ok": ok,
            "mode": mode,
            "records": parsed.get("records", []),
            "serverExit": server.returncode,
            "dllReleasedAfterUnload": released,
            "dll": str(live_dll),
        }
        if not ok:
            report.update({
                "result": result,
                "comExit": completed.returncode,
                "comStdout": completed.stdout,
                "comStderr": completed.stderr,
                "serverStdout": server_stdout,
                "serverStderr": server_stderr,
            })
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
