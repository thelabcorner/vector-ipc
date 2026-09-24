#target illustrator

(function () {
    var probeArgs = $.global.__com_args;
    var nativeDir = probeArgs && probeArgs.length
        ? String(probeArgs[0])
        : "";
    var probeName = probeArgs && probeArgs.length > 1
        ? String(probeArgs[1])
        : "VectorIPCExternalObjectProbe";
    var endpoint = probeArgs && probeArgs.length > 2
        ? String(probeArgs[2])
        : "vipcprobe1";
    var oldSearch = ExternalObject.searchFolders;
    var lib = null;

    function packString6(value, start) {
        var packed = 0;
        var factor = 1;
        var i;
        for (i = 0; i < 6 && (start + i) < value.length; i++) {
            packed += (value.charCodeAt(start + i) & 255) * factor;
            factor *= 256;
        }
        return packed;
    }

    if (!nativeDir || !endpoint) {
        throw new Error("native DLL directory/endpoint argument missing");
    }

    ExternalObject.searchFolders = nativeDir + ";" + oldSearch;

    try {
        var found = ExternalObject.search("lib:" + probeName);
        if (!found) throw new Error("VectorIPC probe DLL not found in " + nativeDir);

        lib = new ExternalObject("lib:" + probeName);
        if (typeof lib.vipc !== "function") {
            throw new Error("vipc method did not bind");
        }

        var info = String(lib.vipc(0));
        if (info !== "VIPC/1.0|INFO|3|1|0|262144|6|16") {
            throw new Error("INFO mismatch: " + info);
        }

        var connectArgs = [1, 2000, endpoint.length];
        var offset;
        for (offset = 0; offset < endpoint.length; offset += 6) {
            connectArgs.push(packString6(endpoint, offset));
        }
        var connected = String(lib.vipc.apply(lib, connectArgs));
        if (connected.indexOf("VIPC/1.0|OK|CONNECTED|") !== 0) {
            throw new Error("CONNECT failed: " + connected);
        }
        var connectedParts = connected.split("|");
        var handle = Number(connectedParts[3]);
        if (!(handle > 0)) {
            throw new Error("invalid session handle: " + connected);
        }

        var reset = String(lib.vipc(2, handle));
        if (reset !== "VIPC/1.0|OK|STAGE_RESET") {
            throw new Error("STAGE_RESET failed: " + reset);
        }

        // Raw payload: 00 01 02 FF 10 80 7F.
        var staged = String(lib.vipc(
            3, handle, 7,
            140810486153472,
            127
        ));
        if (staged !== "VIPC/1.0|OK|STAGED|7") {
            throw new Error("STAGE_APPEND failed: " + staged);
        }

        var response = String(lib.vipc(
            4,
            handle,
            2000,
            326,
            2882400001,
            305419896
        ));
        var expected =
            "VIPC/1.0|FRAME|2|0|326|2882400001|305419896|7|AAEC/xCAfw==";
        if (response !== expected) {
            throw new Error("TRANSACT mismatch: " + response);
        }

        var closed = String(lib.vipc(7, handle));
        if (closed !== "VIPC/1.0|OK|CLOSED") {
            throw new Error("CLOSE failed: " + closed);
        }

        var stale = String(lib.vipc(2, handle));
        if (stale !== "VIPC/1.0|ERR|STATE|INVALID_HANDLE") {
            throw new Error("stale handle was accepted: " + stale);
        }

        return "PASS|handle=" + handle + "|" + info + "|" + response;
    } finally {
        try {
            if (lib) lib.unload();
        } catch (ignoreUnload) {}
        ExternalObject.searchFolders = oldSearch;
    }
}());
