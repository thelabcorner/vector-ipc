#target illustrator

(function () {
    var a = $.global.__com_args;
    var nativeDir = String(a[0]);
    var wrapperPath = String(a[1]);
    var probeName = String(a[2]);
    var endpoint = String(a[3]);
    var esonPath = String(a[4]);
    var esb64Path = String(a[5]);
    var escharsPath = String(a[6]);
    var client = null;

    $.global.VectorIPCExternalObject = undefined;
    $.global.VectorIPC = undefined;
    $.global.ESON = undefined;
    $.global.ESB64 = undefined;
    $.global.ESCHARS = undefined;

    $.evalFile(new File(wrapperPath), 10000);
    $.evalFile(new File(esonPath), 10000);
    $.evalFile(new File(esb64Path), 10000);
    $.evalFile(new File(escharsPath), 10000);

    if (typeof VectorIPC === "undefined"
        || typeof ESON === "undefined"
        || typeof ESB64 === "undefined"
        || typeof ESCHARS === "undefined") {
        throw new Error("ecosystem globals did not initialize");
    }
    if (!ESCHARS.espack || !ESCHARS.espack.ok) {
        throw new Error("ESCHARS native accel did not initialize: "
            + String(ESCHARS.espack && ESCHARS.espack.reason));
    }

    try {
        client = VectorIPC.load(nativeDir, probeName);
        client.connect(endpoint, 3000);

        var structured = {
            kind: "ecosystem",
            nested: { ok: true, count: 3 },
            labels: ["CutContour", "dieline", "世界"],
            number: 42
        };
        var er = client.requestESON(
            0x180,
            structured,
            3000,
            { low: 0x1234, high: 0 }
        );
        if (!er.value
            || er.value.kind !== "ecosystem"
            || er.value.nested.ok !== true
            || er.value.nested.count !== 3
            || er.value.labels.length !== 3
            || er.value.labels[2] !== "世界"
            || er.value.number !== 42) {
            throw new Error("ESON round-trip mismatch");
        }

        var raw = String.fromCharCode(0, 1, 2, 255, 16, 128, 127);
        var br = client.requestBinaryString(
            0x181,
            raw,
            3000,
            { low: 0x5678, high: 0 },
            64
        );

        var decoded = VectorIPC.decodeBase64BinaryString(br.payloadBase64);
        if (decoded.length !== raw.length) {
            throw new Error("ESB64 decoded length mismatch");
        }
        var i;
        for (i = 0; i < raw.length; i++) {
            if (decoded.charCodeAt(i) !== raw.charCodeAt(i)) {
                throw new Error("ESB64 byte mismatch at " + i);
            }
        }

        var hex = VectorIPC.decodeBase64Hex(br.payloadBase64);
        if (hex !== "000102ff10807f") {
            throw new Error("ESCHARS hex mismatch: " + hex);
        }

        return [
            "PASS",
            "wrapper=" + VectorIPC.version,
            "adapter=" + VectorIPC.adapterVersion,
            "eson=ok",
            "esb64=" + String(ESB64.acceleration || "es3"),
            "eschars=" + String(ESCHARS.espack.mode || "native"),
            "hex=" + hex
        ].join("|");
    } finally {
        if (client) client.dispose();
        try { if (ESCHARS && ESCHARS.unload) ESCHARS.unload(); } catch (ignore1) {}
    }
}());