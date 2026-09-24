"""Benchmark VectorIPC requestText inside live Illustrator."""
from __future__ import annotations
import argparse, json, os, subprocess, sys
from pathlib import Path

from live_support import stage_live_dll, try_remove_live_dll

def main() -> int:
    ap=argparse.ArgumentParser()
    ap.add_argument("--size",type=int,required=True)
    ap.add_argument("--samples",type=int,default=10)
    ns=ap.parse_args()
    repo=Path(__file__).resolve().parent.parent
    skill=repo.parent/"agent-skills"/"illustrator-com-automation-skill"
    tool=skill/"comtool"/"ILLUSTRATOR_COM_TOOL.py"
    probe=repo/"tests"/"illustrator_text_bench.jsx"
    wrapper=repo/"src"/"adapters"/"externalobject"/"VectorIPC.jsx"
    dll=repo/"build"/"Release"/"VectorIPCExternalObject.dll"
    serverexe=repo/"build"/"Release"/"vectoripc_matrix_bench.exe"
    live,name,live_dll=stage_live_dll(dll,"VectorIPCTextBench")
    endpoint=f"eot{os.getpid()%1_000_000_000:09d}"
    iterations=ns.samples+2
    flags=getattr(subprocess,"CREATE_NO_WINDOW",0)
    server=subprocess.Popen([str(serverexe),"--server",endpoint,str(iterations)],cwd=repo,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,creationflags=flags)
    try:
        cp=subprocess.run([sys.executable,str(tool),"eval","--file",str(probe),"--args-json",json.dumps([live.as_posix(),name,endpoint,wrapper.as_posix(),ns.size,ns.samples]),"--timeout","30"],cwd=skill,capture_output=True,text=True,timeout=40,check=False)
        so,se=server.communicate(timeout=10)
        lines=[x.strip() for x in cp.stdout.splitlines() if x.strip()]
        env={}
        if lines:
            try: env=json.loads(lines[-1])
            except json.JSONDecodeError: pass
        result=env.get("result")
        if isinstance(result,dict) and "result" in result: result=result["result"]
        ok=cp.returncode==0 and env.get("ok") is True and isinstance(result,str) and result.startswith("PASS|") and server.returncode==0
        released=try_remove_live_dll(live_dll)
        print(json.dumps({"ok":ok,"result":result,"serverExit":server.returncode,"dllReleasedAfterUnload":released,"dll":str(live_dll),"stderr":cp.stderr if not ok else "","serverStderr":se if not ok else ""},ensure_ascii=False))
        return 0 if ok else 1
    finally:
        if server.poll() is None:
            server.kill(); server.wait(timeout=2)
        if live_dll.exists():
            try_remove_live_dll(live_dll)

if __name__=="__main__": raise SystemExit(main())
