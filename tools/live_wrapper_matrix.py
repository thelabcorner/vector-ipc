"""Benchmark the complete ES3 wrapper path across payload and chunk sizes."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from live_support import stage_live_dll, try_remove_live_dll


FULL_CASES: tuple[tuple[int, int, int, int], ...] = (
    (0, 256, 10, 50),
    (32, 256, 10, 50),
    (1024, 256, 10, 50),
    (4096, 256, 5, 30),
    (16384, 256, 3, 15),
    (65536, 256, 1, 5),
    (262144, 256, 0, 2),
    (65536, 32, 1, 3),
    (65536, 64, 1, 3),
    (65536, 128, 1, 3),
    (65536, 256, 1, 3),
)

QUICK_CASES: tuple[tuple[int, int, int, int], ...] = (
    (0, 256, 2, 10),
    (1024, 256, 2, 5),
    (4096, 256, 1, 3),
    (4096, 64, 0, 1),
    (4096, 256, 0, 1),
)


def _raw_result(envelope: dict) -> object:
    value = envelope.get("result")
    if isinstance(value, dict) and "result" in value:
        return value["result"]
    return value


def _parse_result(result: str) -> list[dict[str, float | int]]:
    if not result.startswith("PASS|"):
        return []

    rows: list[dict[str, float | int]] = []
    payload = result[5:]
    if not payload:
        return rows

    for raw_row in payload.split(";"):
        fields = raw_row.split(",")
        if len(fields) != 11:
            return []
        rows.append(
            {
                "size": int(fields[0]),
                "chunkWords": int(fields[1]),
                "samples": int(fields[2]),
                "stageMedianUs": float(fields[3]),
                "stageP95Us": float(fields[4]),
                "transactMedianUs": float(fields[5]),
                "transactP95Us": float(fields[6]),
                "decodeMedianUs": float(fields[7]),
                "decodeP95Us": float(fields[8]),
                "totalMedianUs": float(fields[9]),
                "totalP95Us": float(fields[10]),
            }
        )
    return rows


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--full",
        action="store_true",
        help="run the exhaustive legacy ES3 payload/chunk matrix",
    )
    args = parser.parse_args()
    mode = "full" if args.full else "quick"
    cases = FULL_CASES if args.full else QUICK_CASES

    repo = Path(__file__).resolve().parent.parent
    scripts_root = repo.parent
    com_skill = scripts_root / "agent-skills" / "illustrator-com-automation-skill"
    tool = com_skill / "comtool" / "ILLUSTRATOR_COM_TOOL.py"
    probe = repo / "tests" / "illustrator_wrapper_matrix_bench.jsx"
    wrapper = repo / "src" / "adapters" / "externalobject" / "VectorIPC.jsx"
    source_dll = repo / "build" / "Release" / "VectorIPCExternalObject.dll"
    echo_server = repo / "build" / "Release" / "vectoripc_matrix_bench.exe"

    for required in (tool, probe, wrapper, source_dll, echo_server):
        if not required.is_file():
            raise FileNotFoundError(required)

    total_iterations = sum(warmup + samples for _, _, warmup, samples in cases)
    live_dir, probe_name, live_dll = stage_live_dll(
        source_dll,
        "VectorIPCExternalObjectMatrix",
    )
    endpoint = f"eom{os.getpid() % 1_000_000_000:09d}"
    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)

    server = subprocess.Popen(
        [str(echo_server), "--server", endpoint, str(total_iterations)],
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
                        endpoint,
                        [list(case) for case in cases],
                    ]
                ),
                "--timeout",
                "180" if args.full else "30",
            ],
            cwd=com_skill,
            capture_output=True,
            text=True,
            check=False,
            timeout=210 if args.full else 45,
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
        rows = _parse_result(result) if isinstance(result, str) else []
        ok = (
            completed.returncode == 0
            and envelope.get("ok") is True
            and len(rows) == len(cases)
            and server.returncode == 0
        )

        released = try_remove_live_dll(live_dll)

        report: dict[str, object] = {
            "ok": ok,
            "mode": mode,
            "rows": rows,
            "serverExit": server.returncode,
            "dllReleasedAfterUnload": released,
            "dll": str(live_dll),
        }
        if not ok:
            report.update(
                {
                    "result": result,
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