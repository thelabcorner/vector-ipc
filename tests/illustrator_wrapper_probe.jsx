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
    var endpoint = probeArgs && probeArgs.length > 3
        ? String(probeArgs[3])
        : "vipcprobe1";
    var client = null;
    var payload = [];
    var decoded;
    var response;
    var info;
    var i;
    var requestStart;
    var requestMs;
    var decodeStart;
    var decodeMs;
    var textResponse;
    var textPayload;

    if (!nativeDir || !wrapperPath || !endpoint) {
        throw new Error("wrapper probe arguments missing");
    }

    /*
     * Persistent COM engines can retain an older wrapper global across source
     * rebuilds. Clear only these library symbols so this probe certifies disk.
     */
    $.global.VectorIPCExternalObject = undefined;
    $.global.VectorIPC = undefined;
    $.evalFile(new File(wrapperPath), 10000);

    if (typeof VectorIPCExternalObject === "undefined"
        || typeof VectorIPC === "undefined") {
        throw new Error("VectorIPC wrapper did not initialize");
    }
    if (VectorIPC !== VectorIPCExternalObject) {
        throw new Error("VectorIPC compatibility alias mismatch");
    }

    try {
        client = VectorIPC.load(nativeDir, probeName);
        info = client.capabilities || client.info();

        if (VectorIPC.version !== 5
            || info.adapterVersion !== 3
            || info.wireMajor !== 1
            || info.packBytes !== 6
            || info.maxPayload !== 262144
            || info.maxSessions !== 16
            || VectorIPC.defaultChunkWords !== 256
            || VectorIPC.maxChunkWords !== 256
            || VectorIPC.supportsESON !== true
            || VectorIPC.supportsESB64 !== true
            || VectorIPC.supportsESCHARS !== true) {
            throw new Error("wrapper compatibility negotiation mismatch");
        }

        client.connect(endpoint, 2000);

        try {
            client.stageByteArray([1], VectorIPC.maxChunkWords + 1);
            throw new Error("oversized chunkWords was accepted");
        } catch (chunkError) {
            if (String(chunkError).indexOf("range 1..256") < 0) {
                throw chunkError;
            }
        }

        try {
            client.requestText(
                0x18f,
                String.fromCharCode(0xd800),
                2000,
                { low: 0x7001, high: 0 }
            );
            throw new Error("lone surrogate text was accepted");
        } catch (surrogateError) {
            if (String(surrogateError).indexOf("unpaired UTF-16 surrogate") < 0) {
                throw surrogateError;
            }
        }

        for (i = 0; i < 1024; i++) {
            payload.push((i * 37 + 11) & 255);
        }

        requestStart = (new Date()).getTime();
        response = client.requestByteArray(
            326,
            payload,
            2000,
            { low: 2882400001, high: 305419896 },
            128
        );
        requestMs = (new Date()).getTime() - requestStart;

        if (response.type !== "frame"
            || response.kind !== 2
            || response.operation !== 326
            || response.correlationLow !== 2882400001
            || response.correlationHigh !== 305419896
            || response.payloadSize !== 1024) {
            throw new Error("wrapper frame metadata mismatch");
        }

        decodeStart = (new Date()).getTime();
        decoded = VectorIPC.decodeBase64(response.payloadBase64);
        if (!decoded || decoded.length !== payload.length) {
            throw new Error("wrapper decoded payload length mismatch");
        }
        for (i = 0; i < payload.length; i++) {
            if (decoded[i] !== payload[i]) {
                throw new Error("wrapper payload mismatch at byte " + i);
            }
        }
        decodeMs = (new Date()).getTime() - decodeStart;

        textPayload = "h"
            + String.fromCharCode(0x00e9)
            + "llo|"
            + String.fromCharCode(0x4e16)
            + String.fromCharCode(0x754c)
            + String.fromCharCode(0xd83d, 0xde00);
        textResponse = client.requestText(
            327,
            textPayload,
            2000,
            { low: 123, high: 456 }
        );
        if (textResponse.type !== "text"
            || textResponse.kind !== 2
            || textResponse.operation !== 327
            || textResponse.correlationLow !== 123
            || textResponse.correlationHigh !== 456
            || textResponse.payloadSize !== 17
            || textResponse.payloadText !== textPayload) {
            throw new Error("wrapper text-lane mismatch: " + textResponse.raw);
        }

        return [
            "PASS",
            "1024",
            "handle=" + client.handle,
            "requestMs=" + requestMs,
            "decodeMs=" + decodeMs,
            "b64=" + response.payloadBase64.length,
            "textBytes=" + textResponse.payloadSize
        ].join("|");
    } finally {
        if (client) client.dispose();
    }
}());
