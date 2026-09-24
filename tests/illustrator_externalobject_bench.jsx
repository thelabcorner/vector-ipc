#target illustrator

(function () {
    var probeArgs = $.global.__com_args;
    var nativeDir = probeArgs && probeArgs.length
        ? String(probeArgs[0])
        : "";
    var probeName = probeArgs && probeArgs.length > 1
        ? String(probeArgs[1])
        : "VectorIPCExternalObjectBench";
    var endpoint = probeArgs && probeArgs.length > 2
        ? String(probeArgs[2])
        : "vipcbench1";
    var warmupCount = 200;
    var sampleCount = 2000;
    var oldSearch = ExternalObject.searchFolders;
    var lib = null;
    var samples = [];
    var i;
    var response;
    var elapsed;
    var sum = 0;

    function packString6(value, start) {
        var packed = 0;
        var factor = 1;
        var index;
        for (index = 0; index < 6 && (start + index) < value.length; index++) {
            packed += (value.charCodeAt(start + index) & 255) * factor;
            factor *= 256;
        }
        return packed;
    }

    if (!nativeDir || !endpoint) {
        throw new Error("native DLL directory/endpoint argument missing");
    }

    ExternalObject.searchFolders = nativeDir + ";" + oldSearch;

    function assertFrame(value) {
        if (value.indexOf("VIPC/1.0|FRAME|2|0|326|") !== 0) {
            throw new Error("unexpected response: " + value);
        }
        if (value.indexOf("|7|AAEC/xCAfw==") < 0) {
            throw new Error("payload mismatch: " + value);
        }
    }

    function numericSort(a, b) {
        return a - b;
    }

    try {
        if (!ExternalObject.search("lib:" + probeName)) {
            throw new Error("probe DLL not found");
        }

        lib = new ExternalObject("lib:" + probeName);
        if (typeof lib.vipc !== "function") {
            throw new Error("vipc method did not bind");
        }

        response = String(lib.vipc(0));
        if (response !== "VIPC/1.0|INFO|3|1|0|262144|6|16") {
            throw new Error("INFO mismatch: " + response);
        }

        var connectArgs = [1, 2000, endpoint.length];
        var offset;
        for (offset = 0; offset < endpoint.length; offset += 6) {
            connectArgs.push(packString6(endpoint, offset));
        }
        response = String(lib.vipc.apply(lib, connectArgs));
        if (response.indexOf("VIPC/1.0|OK|CONNECTED|") !== 0) {
            throw new Error("CONNECT failed: " + response);
        }
        var connectedParts = response.split("|");
        var handle = Number(connectedParts[3]);
        if (!(handle > 0)) {
            throw new Error("invalid session handle: " + response);
        }

        response = String(lib.vipc(2, handle));
        if (response !== "VIPC/1.0|OK|STAGE_RESET") {
            throw new Error("STAGE_RESET failed: " + response);
        }

        response = String(lib.vipc(
            3, handle, 7,
            140810486153472,
            127
        ));
        if (response !== "VIPC/1.0|OK|STAGED|7") {
            throw new Error("STAGE_APPEND failed: " + response);
        }

        for (i = 0; i < warmupCount; i++) {
            response = String(lib.vipc(4, handle, 2000, 326, i + 1, 0));
            assertFrame(response);
        }

        for (i = 0; i < sampleCount; i++) {
            // $.hiresTimer measures microseconds since its previous access.
            $.hiresTimer;
            response = String(lib.vipc(
                4,
                handle,
                2000,
                326,
                warmupCount + i + 1,
                0
            ));
            elapsed = $.hiresTimer;
            assertFrame(response);

            if (elapsed < 0 || elapsed > 1000000) {
                throw new Error("implausible hiresTimer sample: " + elapsed);
            }
            samples.push(elapsed);
            sum += elapsed;
        }

        samples.sort(numericSort);

        var median = samples[Math.floor(sampleCount / 2)];
        var p95 = samples[Math.floor((sampleCount - 1) * 0.95)];
        var minimum = samples[0];
        var maximum = samples[sampleCount - 1];
        var mean = sum / sampleCount;

        response = String(lib.vipc(7, handle));
        if (response !== "VIPC/1.0|OK|CLOSED") {
            throw new Error("CLOSE failed: " + response);
        }

        return [
            "PASS",
            "adapter=3",
            "handle=" + handle,
            "n=" + sampleCount,
            "warmup=" + warmupCount,
            "min_us=" + minimum,
            "median_us=" + median,
            "p95_us=" + p95,
            "mean_us=" + mean,
            "max_us=" + maximum
        ].join("|");
    } finally {
        try {
            if (lib) lib.unload();
        } catch (ignoreUnload) {}
        ExternalObject.searchFolders = oldSearch;
    }
}());
