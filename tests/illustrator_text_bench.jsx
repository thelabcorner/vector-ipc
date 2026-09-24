#target illustrator

(function () {
    var args = $.global.__com_args;
    var nativeDir = String(args[0]);
    var probeName = String(args[1]);
    var endpoint = String(args[2]);
    var wrapperPath = String(args[3]);
    var size = Number(args[4]);
    var samples = Number(args[5]);
    var oldSearch = ExternalObject.searchFolders;
    var client = null;
    var times = [];
    var payload = new Array(size + 1).join("A");
    var i;

    function sortNum(a, b) { return a - b; }

    $.global.VectorIPCExternalObject = undefined;
    $.global.VectorIPC = undefined;
    $.evalFile(new File(wrapperPath), 10000);
    ExternalObject.searchFolders = nativeDir + ";" + oldSearch;

    try {
        client = VectorIPCExternalObject.load(nativeDir, probeName);
        client.connect(endpoint, 5000);

        for (i = 0; i < 2; i++) {
            var warm = client.requestText(350, payload, 5000);
            if (warm.payloadText !== payload) throw new Error("warm mismatch");
        }

        for (i = 0; i < samples; i++) {
            $.hiresTimer;
            var frame = client.requestText(350, payload, 5000);
            var us = $.hiresTimer;
            if (frame.payloadText !== payload || frame.payloadSize !== size) {
                throw new Error("text mismatch");
            }
            times.push(us);
        }

        times.sort(sortNum);
        var sum = 0;
        for (i = 0; i < times.length; i++) sum += times[i];

        return [
            "PASS",
            "size=" + size,
            "n=" + samples,
            "mean_us=" + Math.round(sum / times.length),
            "median_us=" + times[Math.floor(times.length / 2)],
            "p95_us=" + times[Math.floor((times.length - 1) * 0.95)],
            "max_us=" + times[times.length - 1]
        ].join("|");
    } finally {
        try { if (client) client.dispose(); } catch (ignore) {}
        ExternalObject.searchFolders = oldSearch;
    }
}());