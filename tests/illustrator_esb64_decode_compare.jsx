#target illustrator
(function () {
    var a = $.global.__com_args;
    var adobe = String(a[0]);
    var esb64 = String(a[1]);
    $.evalFile(File(adobe));
    $.evalFile(File(esb64));

    function repeat(s, n) {
        var parts = [];
        var i;
        for (i = 0; i < n; i++) parts[parts.length] = s;
        return parts.join("");
    }
    function time(fn) {
        $.hiresTimer;
        var v = fn();
        var us = $.hiresTimer;
        return { us: us, value: v };
    }

    var blocks = 21845; // 65,535 decoded bytes
    var clean = repeat("QUJD", blocks); // ABC
    var nul = repeat("AAEC", blocks);   // 00 01 02

    var aClean = time(function () { return VectorIPC.decodeBase64(clean); });
    var eClean = time(function () { return ESB64.atob(clean); });
    var aNul = time(function () { return VectorIPC.decodeBase64(nul); });
    var eNul = time(function () { return ESB64.atob(nul); });

    if (aClean.value.length !== 65535 || eClean.value.length !== 65535 ||
        aNul.value.length !== 65535 || eNul.value.length !== 65535) {
        throw new Error("length mismatch");
    }
    if (aClean.value[0] !== 65 || eClean.value.charCodeAt(0) !== 65 ||
        aNul.value[0] !== 0 || eNul.value.charCodeAt(0) !== 0 ||
        eNul.value.charCodeAt(65534) !== 2) {
        throw new Error("content mismatch");
    }

    return [
        "PASS",
        "esb64_mode=" + String(ESB64.acceleration || "es3"),
        "adobe_clean_us=" + aClean.us,
        "esb64_clean_us=" + eClean.us,
        "adobe_nul_us=" + aNul.us,
        "esb64_nul_us=" + eNul.us
    ].join("|");
}());