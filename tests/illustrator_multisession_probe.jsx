#target illustrator

(function () {
    var probeArgs = $.global.__com_args;
    var nativeDir = probeArgs && probeArgs.length > 0
        ? String(probeArgs[0])
        : "";
    var wrapperPath = probeArgs && probeArgs.length > 1
        ? String(probeArgs[1])
        : "";
    var probeName = probeArgs && probeArgs.length > 2
        ? String(probeArgs[2])
        : "VectorIPCExternalObjectProbe";
    var endpointA = probeArgs && probeArgs.length > 3
        ? String(probeArgs[3])
        : "vipcmsa";
    var endpointB = probeArgs && probeArgs.length > 4
        ? String(probeArgs[4])
        : "vipcmsb";
    var clientA = null;
    var clientB = null;
    var response;
    var oldHandleA;

    if (!nativeDir || !wrapperPath || !endpointA || !endpointB) {
        throw new Error("multi-session probe arguments missing");
    }

    $.global.VectorIPCExternalObject = undefined;
    $.global.VectorIPC = undefined;
    $.evalFile(new File(wrapperPath), 10000);
    if (typeof VectorIPCExternalObject === "undefined"
        || typeof VectorIPC === "undefined"
        || VectorIPC !== VectorIPCExternalObject) {
        throw new Error("VectorIPC wrapper did not initialize");
    }

    try {
        clientA = VectorIPCExternalObject.load(nativeDir, probeName);
        clientB = VectorIPCExternalObject.load(nativeDir, probeName);

        clientA.connect(endpointA, 2000);
        clientB.connect(endpointB, 2000);

        if (!(clientA.handle > 0)
            || !(clientB.handle > 0)
            || clientA.handle === clientB.handle) {
            throw new Error("session handles are not independent");
        }

        clientA.stageBinaryString(String.fromCharCode(0, 161, 162, 255), 2);
        clientB.stageBinaryString(String.fromCharCode(0, 177, 178, 128, 127), 2);

        // Service B first even though A was staged first.
        response = clientB.transact(337, 8193, 0, 2000);
        if (response.payloadBase64 !== "ALGygH8=") {
            throw new Error("session B payload contamination: " + response.raw);
        }

        response = clientA.transact(336, 4097, 0, 2000);
        if (response.payloadBase64 !== "AKGi/w==") {
            throw new Error("session A payload contamination: " + response.raw);
        }

        oldHandleA = clientA.handle;
        clientA.dispose();
        clientA = null;

        // B must keep both its native channel and its staged payload.
        response = clientB.transact(337, 8194, 0, 2000);
        if (response.payloadBase64 !== "ALGygH8=") {
            throw new Error("session B did not survive A disposal: " + response.raw);
        }

        return [
            "PASS",
            "closedA=" + oldHandleA,
            "liveB=" + clientB.handle,
            "payloadB=" + response.payloadBase64
        ].join("|");
    } finally {
        if (clientA) clientA.dispose();
        if (clientB) clientB.dispose();
    }
}());