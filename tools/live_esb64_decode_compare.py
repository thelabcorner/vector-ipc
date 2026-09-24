from __future__ import annotations
import json, subprocess, sys
from pathlib import Path
def main():
    repo=Path(__file__).resolve().parent.parent
    skill=repo.parent/"agent-skills"/"illustrator-com-automation-skill"
    tool=skill/"comtool"/"ILLUSTRATOR_COM_TOOL.py"
    probe=repo/"tests"/"illustrator_esb64_decode_compare.jsx"
    wrapper=repo/"src"/"adapters"/"externalobject"/"VectorIPC.jsx"
    esb64=repo.parent/"esb64"/"dist"/"ESB64.accel.jsx"
    cp=subprocess.run([sys.executable,str(tool),"eval","--file",str(probe),
        "--args-json",json.dumps([wrapper.as_posix(),esb64.as_posix()]),"--timeout","45"],
        cwd=skill,capture_output=True,text=True,timeout=60)
    print(cp.stdout.strip())
    if cp.stderr: print(cp.stderr,file=sys.stderr)
    return cp.returncode
if __name__=="__main__": raise SystemExit(main())
