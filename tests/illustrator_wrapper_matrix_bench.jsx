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
        : "VectorIPCExternalObjectMatrix";
    var endpoint = probeArgs && probeArgs.length > 3
        ? String(probeArgs[3])
        : "vipcwmatrix";
    var rawCases = probeArgs && probeArgs.length > 4 ? probeArgs[4] : null;
    var cases = [];
    var rawCaseIndex;
    if (!rawCases || !rawCases.length) {
        throw new Error("wrapper matrix case list missing");
    }
    for (rawCaseIndex = 0; rawCaseIndex < rawCases.length; rawCaseIndex++) {
        cases.push({
            size: Number(rawCases[rawCaseIndex][0]),
            chunk: Number(rawCases[rawCaseIndex][1]),
            warmup: Number(rawCases[rawCaseIndex][2]),
            samples: Number(rawCases[rawCaseIndex][3])
        });
    }
    var client = null;
    var results = [];
    var caseIndex;

    function numericSort(a, b) {
        return a - b;
    }

    function quantile(sorted, fraction) {
        return sorted[Math.floor((sorted.length - 1) * fraction)];
    }

    function summarize(samples) {
        var sorted = samples.slice(0);
        var sum = 0;
        var i;
        sorted.sort(numericSort);
        for (i = 0; i < sorted.length; i++) sum += sorted[i];
        return {
            median: sorted[Math.floor(sorted.length / 2)],
            p95: quantile(sorted, 0.95),
            mean: sum / sorted.length,
            max: sorted[sorted.length - 1]
        };
    }

    function makePayload(size) {
        var bytes = new Array(size);
        var i;
        for (i = 0; i < size; i++) {
            bytes[i] = (i * 37 + size * 13 + 11) & 255;
        }
        return bytes;
    }

    function verifyPayload(decoded, expected) {
        var i;
        if (!decoded || decoded.length !== expected.length) {
            throw new Error(
                "decoded length mismatch: "
                    + (decoded ? decoded.length : -1)
                    + " != "
                    + expected.length
            );
        }
        for (i = 0; i < expected.length; i++) {
            if (decoded[i] !== expected[i]) {
                throw new Error(
                    "decoded payload mismatch at " + i
                );
            }
        }
    }

    function runOne(payload, chunkWords, operation) {
        var stageUs;
        var transactUs;
        var decodeUs;
        var totalUs;
        var response;
        var decoded;


        $.hiresTimer;
        client.stageByteArray(payload, chunkWords);
        stageUs = $.hiresTimer;

        $.hiresTimer;
        response = client.transact(
            operation,
            client.nextCorrelation().low,
            0,
            5000
        );
        transactUs = $.hiresTimer;

        if (response.type !== "frame"
            || response.kind !== 2
            || response.operation !== operation
            || response.payloadSize !== payload.length) {
            throw new Error("matrix frame metadata mismatch: " + response.raw);
        }

        $.hiresTimer;
        decoded = VectorIPC.decodeBase64(response.payloadBase64);
        decodeUs = $.hiresTimer;

        /*
         * $.hiresTimer is interval-based, so total is the sum of the separately
         * timed contiguous phases. Verification intentionally sits outside.
         */
        totalUs = stageUs + transactUs + decodeUs;
        verifyPayload(decoded, payload);
        return {
            stage: stageUs,
            transact: transactUs,
            decode: decodeUs,
            total: totalUs
        };
    }

    if (!nativeDir || !wrapperPath || !endpoint) {
        throw new Error("wrapper matrix arguments missing");
    }

    $.global.VectorIPCExternalObject = undefined;
    $.global.VectorIPC = undefined;
    $.evalFile(new File(wrapperPath), 10000);

    if (typeof VectorIPC === "undefined"
        || VectorIPC.maxChunkWords !== 256) {
        throw new Error("VectorIPC wrapper/capability mismatch");
    }

    try {
        client = VectorIPC.load(nativeDir, probeName);
        client.connect(endpoint, 5000);

        for (caseIndex = 0; caseIndex < cases.length; caseIndex++) {
            var bench = cases[caseIndex];
            var payload = makePayload(bench.size);
            var stageSamples = [];
            var transactSamples = [];
            var decodeSamples = [];
            var totalSamples = [];
            var i;
            var measured;
            var stageSummary;
            var transactSummary;
            var decodeSummary;
            var totalSummary;

            for (i = 0; i < bench.warmup; i++) {
                runOne(
                    payload,
                    bench.chunk,
                    400 + caseIndex
                );
            }

            for (i = 0; i < bench.samples; i++) {
                try {
                    measured = runOne(
                        payload,
                        bench.chunk,
                        400 + caseIndex
                    );
                } catch (caseError) {
                    throw new Error(
                        "matrix case "
                            + caseIndex
                            + " size="
                            + bench.size
                            + " chunk="
                            + bench.chunk
                            + ": "
                            + String(caseError)
                    );
                }
                stageSamples.push(measured.stage);
                transactSamples.push(measured.transact);
                decodeSamples.push(measured.decode);
                totalSamples.push(measured.total);
            }

            stageSummary = summarize(stageSamples);
            transactSummary = summarize(transactSamples);
            decodeSummary = summarize(decodeSamples);
            totalSummary = summarize(totalSamples);

            results.push([
                bench.size,
                bench.chunk,
                bench.samples,
                stageSummary.median,
                stageSummary.p95,
                transactSummary.median,
                transactSummary.p95,
                decodeSummary.median,
                decodeSummary.p95,
                totalSummary.median,
                totalSummary.p95
            ].join(","));
        }

        return "PASS|" + results.join(";");
    } finally {
        if (client) client.dispose();
    }
}());