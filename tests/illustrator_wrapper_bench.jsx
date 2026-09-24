#target illustrator

(function () {
    var probeArgs = $.global.__com_args;
    var nativeDir = probeArgs && probeArgs.length > 0 ? String(probeArgs[0]) : "";
    var wrapperPath = probeArgs && probeArgs.length > 1 ? String(probeArgs[1]) : "";
    var probeName = probeArgs && probeArgs.length > 2
        ? String(probeArgs[2])
        : "VectorIPCExternalObjectWrapperBench";
    var endpoint = probeArgs && probeArgs.length > 3 ? String(probeArgs[3]) : "";
    var mode = probeArgs && probeArgs.length > 4 ? String(probeArgs[4]) : "quick";
    var cases = mode === "full"
        ? [
            { size: 0,      warmup: 10, samples: 50 },
            { size: 32,     warmup: 10, samples: 50 },
            { size: 1024,   warmup: 5,  samples: 20 },
            { size: 4096,   warmup: 2,  samples: 10 },
            { size: 16384,  warmup: 1,  samples: 3 },
            { size: 65536,  warmup: 0,  samples: 1 },
            { size: 262144, warmup: 0,  samples: 1 }
        ]
        : [
            { size: 0,    warmup: 2, samples: 10 },
            { size: 1024, warmup: 2, samples: 5 },
            { size: 4096, warmup: 1, samples: 3 }
        ];
    var chunkCases = mode === "full" ? [32, 64, 128, 256] : [64, 256];
    var chunkPayloadSize = mode === "full" ? 16384 : 4096;
    var client = null;
    var output = ["PASS", "wrapper=5", "adapter=3"];

    function numericSort(a, b) {
        return a - b;
    }

    function makePayload(size) {
        var bytes = [];
        var i;
        for (i = 0; i < size; i++) {
            bytes.push((i * 37 + size * 11 + 17) & 255);
        }
        return bytes;
    }

    function verifyDecoded(decoded, payload) {
        var i;
        if (!decoded || decoded.length !== payload.length) {
            throw new Error("decoded length mismatch");
        }
        for (i = 0; i < payload.length; i++) {
            if (decoded[i] !== payload[i]) {
                throw new Error("decoded payload mismatch at " + i);
            }
        }
    }

    function summarize(samples) {
        var sorted = samples.slice(0);
        var sum = 0;
        var i;
        sorted.sort(numericSort);
        for (i = 0; i < sorted.length; i++) sum += sorted[i];
        return {
            min: sorted[0],
            median: sorted[Math.floor(sorted.length / 2)],
            p95: sorted[Math.floor((sorted.length - 1) * 0.95)],
            max: sorted[sorted.length - 1],
            mean: sum / sorted.length
        };
    }

    function measureRequest(size, warmup, sampleCount, chunkWords, operation) {
        var payload = makePayload(size);
        var expectedBase64Length = Math.ceil(size / 3) * 4;
        var response = null;
        var samples = [];
        var decodeSamples = [];
        var elapsed;
        var decoded;
        var i;
        var summary;
        var decodeSummary;

        for (i = 0; i < warmup; i++) {
            response = client.requestByteArray(
                operation,
                payload,
                5000,
                null,
                chunkWords
            );
            if (response.payloadSize !== size
                || response.payloadBase64.length !== expectedBase64Length) {
                throw new Error("warmup response mismatch size=" + size);
            }
        }

        for (i = 0; i < sampleCount; i++) {
            $.hiresTimer;
            response = client.requestByteArray(
                operation,
                payload,
                5000,
                null,
                chunkWords
            );
            elapsed = $.hiresTimer;
            if (response.payloadSize !== size
                || response.payloadBase64.length !== expectedBase64Length) {
                throw new Error("response mismatch size=" + size);
            }
            samples.push(elapsed);
        }

        if (!response) {
            response = client.requestByteArray(
                operation,
                payload,
                5000,
                null,
                chunkWords
            );
        }

        for (i = 0; i < 3; i++) {
            $.hiresTimer;
            decoded = VectorIPC.decodeBase64(response.payloadBase64);
            elapsed = $.hiresTimer;
            decodeSamples.push(elapsed);
        }
        verifyDecoded(decoded, payload);

        summary = summarize(samples);
        decodeSummary = summarize(decodeSamples);
        return {
            size: size,
            chunkWords: chunkWords,
            samples: sampleCount,
            min: summary.min,
            median: summary.median,
            p95: summary.p95,
            mean: summary.mean,
            max: summary.max,
            decodeMedian: decodeSummary.median
        };
    }

    function pushResult(prefix, result) {
        output.push(
            prefix
                + ":size=" + result.size
                + ",chunk=" + result.chunkWords
                + ",n=" + result.samples
                + ",min=" + result.min
                + ",median=" + result.median
                + ",p95=" + result.p95
                + ",mean=" + result.mean
                + ",max=" + result.max
                + ",decodeMedian=" + result.decodeMedian
        );
    }

    if (!nativeDir || !wrapperPath || !endpoint) {
        throw new Error("wrapper benchmark arguments missing");
    }

    $.global.VectorIPCExternalObject = undefined;
    $.global.VectorIPC = undefined;
    $.evalFile(new File(wrapperPath), 10000);

    if (typeof VectorIPC === "undefined"
        || VectorIPC.version !== 5
        || VectorIPC.maxChunkWords !== 256) {
        throw new Error("wrapper capability mismatch");
    }

    try {
        var i;
        var result;
        client = VectorIPC.load(nativeDir, probeName);
        client.connect(endpoint, 5000);

        for (i = 0; i < cases.length; i++) {
            result = measureRequest(
                cases[i].size,
                cases[i].warmup,
                cases[i].samples,
                VectorIPC.defaultChunkWords,
                400
            );
            pushResult("payload", result);
        }

        for (i = 0; i < chunkCases.length; i++) {
            result = measureRequest(
                chunkPayloadSize,
                0,
                1,
                chunkCases[i],
                401
            );
            pushResult("chunk", result);
        }

        return output.join("|");
    } finally {
        if (client) client.dispose();
    }
}());
