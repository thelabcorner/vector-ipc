"""Run the minimal VectorIPC ExternalObject adapter-v3 INFO probe."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys

from live_support import stage_live_dll, try_remove_live_dll


EXPECTED = "VIPC/1.0|INFO|3|1|0|262144|6|16"


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
    probe = repo / "tests" / "live_externalobject_info_probe.jsx"
    source_dll = repo / "build" / "Release" / "VectorIPCExternalObject.dll"

    for required in (tool, probe, source_dll):
        if not required.is_file():
            raise FileNotFoundError(required)

    _live_dir, _probe_name, live_dll = stage_live_dll(
        source_dll,
        "VectorIPCExternalObjectInfo",
    )

    completed = subprocess.run(
        [
            sys.executable,
            str(tool),
            "eval",
            "--file",
            str(probe),
            "--args-json",
            json.dumps([live_dll.as_posix()]),
            "--timeout",
            "10",
        ],
        cwd=com_skill,
        capture_output=True,
        text=True,
        check=False,
        timeout=20,
    )

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
        and result == EXPECTED
    )

    released = try_remove_live_dll(live_dll)

    report = {
        "ok": ok,
        "result": result,
        "comExit": completed.returncode,
        "dllReleasedAfterUnload": released,
        "dll": str(live_dll),
    }
    if not ok:
        report["comStdout"] = completed.stdout
        report["comStderr"] = completed.stderr

    print(json.dumps(report, ensure_ascii=False))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
