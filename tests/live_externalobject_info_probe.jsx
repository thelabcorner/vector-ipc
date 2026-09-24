/*
 * Disposable live-host probe for VectorIPCExternalObject.
 * Directive-free on purpose: the COM tool injects this as a wrapper body and
 * supplies the numbered DLL path through arguments[0].
 */
var __vipcLib = null;
var __vipcResult = "";
try {
    __vipcLib = new ExternalObject("lib:" + String(arguments[0]));
    var __vipcDirect = String(__vipcLib.vipc(0));
    var __vipcApplied = String(__vipcLib.vipc.apply(__vipcLib, [0]));
    if (__vipcDirect === __vipcApplied) {
        __vipcResult = __vipcDirect;
    } else {
        __vipcResult =
            "PROBE_ERR|APPLY_MISMATCH|" + __vipcDirect + "|" + __vipcApplied;
    }
} catch (__vipcError) {
    __vipcResult = "PROBE_ERR|" + String(__vipcError);
}
try {
    if (__vipcLib && __vipcLib.unload) {
        __vipcLib.unload();
    }
} catch (__vipcUnloadError) {}
return __vipcResult;
