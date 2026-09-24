"""Live Illustrator certification of VectorIPC + ESON/ESB64/ESCHARS composition."""
from __future__ import annotations
import json, os, subprocess, sys
from pathlib import Path

from live_support import stage_live_dll, try_remove_live_dll

def raw_result(env: dict):
    value = env.get("result")
    if isinstance(value, dict) and "result" in value:
        return value["result"]
    return value

def main() -> int:
    repo=Path(__file__).resolve().parent.parent
    scripts=repo.parent
    skill=scripts/"agent-skills"/"illustrator-com-automation-skill"
    tool=skill/"comtool"/"ILLUSTRATOR_COM_TOOL.py"
    probe=repo/"tests"/"illustrator_ecosystem_probe.jsx"
    wrapper=repo/"src"/"adapters"/"externalobject"/"VectorIPC.jsx"
    source_dll=repo/"build"/"Release"/"VectorIPCExternalObject.dll"
    server_exe=repo/"build"/"Release"/"vectoripc_ping_bench.exe"
    eson=scripts/"eson"/"dist"/"ESON.jsx"
    esb64=scripts/"esb64"/"dist"/"ESB64.accel.jsx"
    eschars=scripts/"eschars"/"dist"/"ESCHARS.accel.jsx"
    required=(tool,probe,wrapper,source_dll,server_exe,eson,esb64,eschars)
    for p in required:
        if not p.is_file(): raise FileNotFoundError(p)

    live,probe_name,live_dll=stage_live_dll(
        source_dll,"VectorIPCExternalObjectEcosystem"
    )
    endpoint=f"eoe{os.getpid()%1_000_000_000:09d}"
    flags=getattr(subprocess,"CREATE_NO_WINDOW",0)
    server=subprocess.Popen(
        [str(server_exe),"--server",endpoint,"2"],
        cwd=repo,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,
        creationflags=flags
    )
    try:
        cp=subprocess.run([
            sys.executable,str(tool),"eval","--file",str(probe),
            "--args-json",json.dumps([
                live.as_posix(),wrapper.as_posix(),probe_name,endpoint,
                eson.as_posix(),esb64.as_posix(),eschars.as_posix()
            ]),
            "--timeout","20"
        ],cwd=skill,capture_output=True,text=True,timeout=30,check=False)
        try:
            so,se=server.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill(); so,se=server.communicate(timeout=2)
        lines=[x.strip() for x in cp.stdout.splitlines() if x.strip()]
        env={}
        if lines:
            try: env=json.loads(lines[-1])
            except json.JSONDecodeError: pass
        result=raw_result(env)
        ok=(cp.returncode==0 and env.get("ok") is True
            and isinstance(result,str) and result.startswith("PASS|")
            and server.returncode==0)
        released=try_remove_live_dll(live_dll)
        report={"ok":ok,"result":result,"serverExit":server.returncode,
                "dllReleasedAfterUnload":released,"dll":str(live_dll)}
        if not ok:
            report.update({"comStdout":cp.stdout,"comStderr":cp.stderr,
                           "serverStdout":so,"serverStderr":se})
        print(json.dumps(report,ensure_ascii=False))
        return 0 if ok else 1
    finally:
        if server.poll() is None:
            server.kill()
            try: server.wait(timeout=2)
            except subprocess.TimeoutExpired: pass

if __name__=="__main__":
    raise SystemExit(main())
