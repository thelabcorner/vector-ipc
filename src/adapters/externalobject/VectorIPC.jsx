/*
 * VectorIPC ExtendScript wrapper.
 *
 * ES3-compatible by design. The native ExternalObject intentionally exposes one
 * numeric method; this file turns that compact ABI into an ergonomic client.
 */

if (typeof VectorIPCExternalObject === "undefined") {
    VectorIPCExternalObject = (function () {
        var CMD_INFO = 0;
        var CMD_CONNECT = 1;
        var CMD_STAGE_RESET = 2;
        var CMD_STAGE_APPEND = 3;
        var CMD_TRANSACT = 4;
        var CMD_SEND = 5;
        var CMD_RECEIVE = 6;
        var CMD_CLOSE = 7;
        var CMD_TRANSACT_TEXT = 8;

        var DEFAULT_TIMEOUT_MS = 2000;
        var DEFAULT_CHUNK_WORDS = 256;
        var MAX_CHUNK_WORDS = 256;
        var PACK_BYTES = 6;
        var MAX_PAYLOAD = 262144;
        var ADAPTER_VERSION = 3;
        var WIRE_MAJOR = 1;
        var PROTOCOL_ID = "VIPC/1.0";
        var U32_MOD = 4294967296;
        var BASE64_ALPHABET =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        var BASE64_DECODE = (function () {
            var table = new Array(128);
            var i;
            for (i = 0; i < table.length; i++) table[i] = -1;
            for (i = 0; i < BASE64_ALPHABET.length; i++) {
                table[BASE64_ALPHABET.charCodeAt(i)] = i;
            }
            return table;
        }());

        function fail(message, response) {
            var error = new Error(message);
            error.vipc = response || null;
            throw error;
        }

        function resolveESON(codec) {
            var resolved = codec;
            if (!resolved && typeof ESON !== "undefined") resolved = ESON;
            if (!resolved
                || typeof resolved.stringify !== "function"
                || typeof resolved.parse !== "function") {
                fail(
                    "VectorIPC requestESON requires ESON or an ESON-compatible codec",
                    null
                );
            }
            return resolved;
        }

        function resolveESB64(codec) {
            var resolved = codec;
            if (!resolved && typeof ESB64 !== "undefined") resolved = ESB64;
            if (!resolved || typeof resolved.atob !== "function") {
                fail(
                    "VectorIPC decodeBase64BinaryString requires ESB64 or an "
                        + "ESB64-compatible codec",
                    null
                );
            }
            return resolved;
        }

        function resolveESCHARS(codec) {
            var resolved = codec;
            if (!resolved && typeof ESCHARS !== "undefined") resolved = ESCHARS;
            if (!resolved || typeof resolved.b64ToHex !== "function") {
                fail(
                    "VectorIPC decodeBase64Hex requires ESCHARS or an "
                        + "ESCHARS-compatible codec",
                    null
                );
            }
            return resolved;
        }

        function validateTextScalarString(source) {
            var i;
            var code;
            var next;

            if (source.indexOf("\u0000") >= 0) {
                fail("VectorIPC text payload cannot contain NUL", null);
            }

            /*
             * Avoid a per-code-unit loop for ordinary text. Only strings that
             * contain UTF-16 surrogate code units need pair validation.
             *
             * Live Illustrator 30.6.0 testing showed why this must happen
             * before ExternalObject: a lone surrogate was silently normalized
             * to an empty string at the host boundary instead of being
             * rejected.
             */
            if (!/[\uD800-\uDFFF]/.test(source)) return;

            for (i = 0; i < source.length; i++) {
                code = source.charCodeAt(i);
                if (code >= 0xD800 && code <= 0xDBFF) {
                    if (i + 1 >= source.length) {
                        fail("VectorIPC text payload has unpaired UTF-16 surrogate", null);
                    }
                    next = source.charCodeAt(i + 1);
                    if (next < 0xDC00 || next > 0xDFFF) {
                        fail("VectorIPC text payload has unpaired UTF-16 surrogate", null);
                    }
                    i++;
                } else if (code >= 0xDC00 && code <= 0xDFFF) {
                    fail("VectorIPC text payload has unpaired UTF-16 surrogate", null);
                }
            }
        }

        function parseUnsigned(text) {
            var value = Number(text);
            if (isNaN(value)
                || !isFinite(value)
                || value < 0
                || value > 4294967295
                || value !== Math.floor(value)) {
                fail("invalid VectorIPC numeric field: " + text, null);
            }
            return value;
        }

        function parseTextResponse(raw) {
            var prefix = PROTOCOL_ID + "|TEXT|";
            var cursor = prefix.length;
            var fields = [];
            var next;
            var i;
            var result;

            if (raw.indexOf(prefix) !== 0) {
                fail("invalid VectorIPC TEXT response", {
                    raw: raw,
                    ok: false,
                    type: "unknown"
                });
            }

            for (i = 0; i < 6; i++) {
                next = raw.indexOf("|", cursor);
                if (next < 0) {
                    fail("invalid VectorIPC TEXT response", {
                        raw: raw,
                        ok: false,
                        type: "unknown"
                    });
                }
                fields[i] = raw.substring(cursor, next);
                cursor = next + 1;
            }

            result = {
                raw: raw,
                ok: true,
                type: "text",
                kind: parseUnsigned(fields[0]),
                flags: parseUnsigned(fields[1]),
                operation: parseUnsigned(fields[2]),
                correlationLow: parseUnsigned(fields[3]),
                correlationHigh: parseUnsigned(fields[4]),
                payloadSize: parseUnsigned(fields[5]),
                payloadText: raw.substring(cursor)
            };
            return result;
        }

        function parseResponse(text) {
            var raw = String(text);
            var parts;
            var result = {
                raw: raw,
                ok: false,
                type: "unknown"
            };

            if (raw.indexOf(PROTOCOL_ID + "|TEXT|") === 0) {
                return parseTextResponse(raw);
            }

            parts = raw.split("|");
            if (parts.length < 2 || parts[0] !== PROTOCOL_ID) {
                fail("invalid VectorIPC ExternalObject response", result);
            }

            if (parts[1] === "OK") {
                result.ok = true;
                result.type = "ok";
                result.fields = parts.slice(2);
                return result;
            }

            if (parts[1] === "INFO") {
                if (parts.length !== 8) {
                    fail("invalid VectorIPC INFO response", result);
                }
                result.ok = true;
                result.type = "info";
                result.adapterVersion = parseUnsigned(parts[2]);
                result.wireMajor = parseUnsigned(parts[3]);
                result.wireMinor = parseUnsigned(parts[4]);
                result.maxPayload = parseUnsigned(parts[5]);
                result.packBytes = parseUnsigned(parts[6]);
                result.maxSessions = parseUnsigned(parts[7]);
                return result;
            }

            if (parts[1] === "FRAME") {
                if (parts.length !== 9) {
                    fail("invalid VectorIPC FRAME response", result);
                }
                result.ok = true;
                result.type = "frame";
                result.kind = parseUnsigned(parts[2]);
                result.flags = parseUnsigned(parts[3]);
                result.operation = parseUnsigned(parts[4]);
                result.correlationLow = parseUnsigned(parts[5]);
                result.correlationHigh = parseUnsigned(parts[6]);
                result.payloadSize = parseUnsigned(parts[7]);
                result.payloadBase64 = parts[8];
                return result;
            }

            if (parts[1] === "ERR") {
                result.type = "error";
                result.fields = parts.slice(2);
                if (parts.length === 5
                    && !isNaN(Number(parts[2]))
                    && !isNaN(Number(parts[3]))
                    && !isNaN(Number(parts[4]))) {
                    result.status = Number(parts[2]);
                    result.phase = Number(parts[3]);
                    result.platformCode = Number(parts[4]);
                }
                return result;
            }

            fail("unknown VectorIPC ExternalObject response", result);
            return result;
        }

        function requireOK(response, operation) {
            if (!response.ok) {
                fail("VectorIPC " + operation + " failed: " + response.raw, response);
            }
            return response;
        }

        function packBytes(getByte, start, count) {
            var value = 0;
            var multiplier = 1;
            var i;
            var byteValue;

            for (i = 0; i < count; i++) {
                byteValue = getByte(start + i);
                if (byteValue < 0 || byteValue > 255 || byteValue !== Math.floor(byteValue)) {
                    fail("byte out of range at index " + (start + i), null);
                }
                value += byteValue * multiplier;
                multiplier *= 256;
            }
            return value;
        }

        function validateChunkWords(chunkWords) {
            if (chunkWords === undefined) chunkWords = DEFAULT_CHUNK_WORDS;
            chunkWords = Number(chunkWords);
            if (chunkWords < 1
                || chunkWords > MAX_CHUNK_WORDS
                || chunkWords !== Math.floor(chunkWords)) {
                fail(
                    "chunkWords must be an integer in the range 1.."
                        + MAX_CHUNK_WORDS,
                    null
                );
            }
            return chunkWords;
        }

        function endpointByteGetter(endpoint) {
            return function (index) {
                var code = endpoint.charCodeAt(index);
                var isUpper = code >= 65 && code <= 90;
                var isLower = code >= 97 && code <= 122;
                var isDigit = code >= 48 && code <= 57;
                var isPunct = code === 46 || code === 95 || code === 45;
                if (!(isUpper || isLower || isDigit || isPunct)) {
                    fail("invalid VectorIPC endpoint byte at index " + index, null);
                }
                return code;
            };
        }

        function Client(lib) {
            var seed = (new Date()).getTime() % U32_MOD;
            this.lib = lib;
            this.handle = 0;
            this.disposed = false;
            this.correlationLow = Math.floor(seed);
            this.correlationHigh = 0;
            this.capabilities = null;
        }

        Client.prototype._call = function (args) {
            if (this.disposed || !this.lib) {
                fail("VectorIPC client is disposed", null);
            }
            return parseResponse(String(this.lib.vipc.apply(this.lib, args)));
        };

        Client.prototype.info = function () {
            return requireOK(this._call([CMD_INFO]), "INFO");
        };

        Client.prototype.nextCorrelation = function () {
            var result = {
                low: this.correlationLow,
                high: this.correlationHigh
            };

            this.correlationLow += 1;
            if (this.correlationLow >= U32_MOD) {
                this.correlationLow = 0;
                this.correlationHigh += 1;
                if (this.correlationHigh >= U32_MOD) {
                    this.correlationHigh = 0;
                }
            }
            return result;
        };

        Client.prototype.connect = function (endpoint, timeoutMs) {
            var args = [CMD_CONNECT];
            var getByte;
            var i;
            var count;
            var response;
            var handle;

            endpoint = String(endpoint);
            if (endpoint.length < 1 || endpoint.length > 80) {
                fail("VectorIPC endpoint length must be 1..80 bytes", null);
            }
            if (timeoutMs === undefined) timeoutMs = DEFAULT_TIMEOUT_MS;

            if (this.handle) {
                try {
                    this.close();
                } catch (ignoreOldSession) {
                    this.handle = 0;
                }
            }

            args.push(timeoutMs);
            args.push(endpoint.length);
            getByte = endpointByteGetter(endpoint);

            for (i = 0; i < endpoint.length; i += PACK_BYTES) {
                count = endpoint.length - i;
                if (count > PACK_BYTES) count = PACK_BYTES;
                args.push(packBytes(getByte, i, count));
            }

            response = requireOK(this._call(args), "CONNECT");
            if (!response.fields
                || response.fields.length !== 4
                || response.fields[0] !== "CONNECTED") {
                fail("invalid VectorIPC CONNECT response", response);
            }
            handle = parseUnsigned(response.fields[1]);
            if (handle < 1 || handle > 4294967295) {
                fail("invalid VectorIPC session handle", response);
            }
            this.handle = handle;
            response.handle = handle;
            response.peerPid = parseUnsigned(response.fields[2]);
            response.peerSessionId = parseUnsigned(response.fields[3]);
            return response;
        };

        Client.prototype.stageReset = function () {
            if (!this.handle) fail("VectorIPC client is not connected", null);
            return requireOK(
                this._call([CMD_STAGE_RESET, this.handle]),
                "STAGE_RESET"
            );
        };

        Client.prototype._appendGetter = function (
            byteLength,
            getByte,
            chunkWords
        ) {
            var offset = 0;
            var chunkBytes;
            var args;
            var localOffset;
            var count;
            var response;

            if (byteLength < 0
                || byteLength > MAX_PAYLOAD
                || byteLength !== Math.floor(byteLength)) {
                fail("VectorIPC staged payload exceeds adapter limit", null);
            }
            chunkWords = validateChunkWords(chunkWords);

            while (offset < byteLength) {
                chunkBytes = byteLength - offset;
                if (chunkBytes > chunkWords * PACK_BYTES) {
                    chunkBytes = chunkWords * PACK_BYTES;
                }

                args = [CMD_STAGE_APPEND, this.handle, chunkBytes];
                localOffset = 0;
                while (localOffset < chunkBytes) {
                    count = chunkBytes - localOffset;
                    if (count > PACK_BYTES) count = PACK_BYTES;
                    args.push(packBytes(getByte, offset + localOffset, count));
                    localOffset += count;
                }

                response = requireOK(this._call(args), "STAGE_APPEND");
                offset += chunkBytes;
            }

            if (byteLength === 0) {
                return requireOK(
                    this._call([CMD_STAGE_APPEND, this.handle, 0]),
                    "STAGE_APPEND"
                );
            }
            return response;
        };

        Client.prototype._stageGetter = function (
            byteLength,
            getByte,
            chunkWords
        ) {
            chunkWords = validateChunkWords(chunkWords);
            this.stageReset();
            return this._appendGetterAtomic(byteLength, getByte, chunkWords);
        };

        Client.prototype._appendGetterAtomic = function (
            byteLength,
            getByte,
            chunkWords
        ) {
            var error;
            chunkWords = validateChunkWords(chunkWords);
            try {
                return this._appendGetter(byteLength, getByte, chunkWords);
            } catch (caught) {
                error = caught;
                try {
                    this.stageReset();
                } catch (ignoreReset) {}
                throw error;
            }
        };

        Client.prototype.stageBinaryString = function (value, chunkWords) {
            var source = String(value);
            return this._stageGetter(
                source.length,
                function (index) {
                    return source.charCodeAt(index);
                },
                chunkWords
            );
        };

        Client.prototype.stageByteArray = function (bytes, chunkWords) {
            if (!bytes || bytes.length === undefined) {
                fail("stageByteArray requires an array-like byte source", null);
            }
            return this._stageGetter(
                Number(bytes.length),
                function (index) {
                    return Number(bytes[index]);
                },
                chunkWords
            );
        };

        Client.prototype.appendBinaryString = function (value, chunkWords) {
            var source = String(value);
            return this._appendGetterAtomic(
                source.length,
                function (index) {
                    return source.charCodeAt(index);
                },
                chunkWords
            );
        };

        Client.prototype.appendByteArray = function (bytes, chunkWords) {
            if (!bytes || bytes.length === undefined) {
                fail("appendByteArray requires an array-like byte source", null);
            }
            return this._appendGetterAtomic(
                Number(bytes.length),
                function (index) {
                    return Number(bytes[index]);
                },
                chunkWords
            );
        };

        Client.prototype.transact = function (
            operation,
            correlationLow,
            correlationHigh,
            timeoutMs
        ) {
            if (timeoutMs === undefined) timeoutMs = DEFAULT_TIMEOUT_MS;
            if (!this.handle) fail("VectorIPC client is not connected", null);
            return requireOK(this._call([
                CMD_TRANSACT,
                this.handle,
                timeoutMs,
                operation,
                correlationLow,
                correlationHigh
            ]), "TRANSACT");
        };

        Client.prototype.requestByteArray = function (
            operation,
            bytes,
            timeoutMs,
            correlation,
            chunkWords
        ) {
            var pair = correlation || this.nextCorrelation();
            var response;

            if (!pair
                || pair.low === undefined
                || pair.high === undefined) {
                fail("invalid VectorIPC correlation pair", null);
            }

            this.stageByteArray(bytes, chunkWords);
            response = this.transact(
                operation,
                Number(pair.low),
                Number(pair.high),
                timeoutMs
            );
            response.correlation = {
                low: Number(pair.low),
                high: Number(pair.high)
            };
            return response;
        };

        Client.prototype.requestBinaryString = function (
            operation,
            value,
            timeoutMs,
            correlation,
            chunkWords
        ) {
            var pair = correlation || this.nextCorrelation();
            var response;

            if (!pair
                || pair.low === undefined
                || pair.high === undefined) {
                fail("invalid VectorIPC correlation pair", null);
            }

            this.stageBinaryString(value, chunkWords);
            response = this.transact(
                operation,
                Number(pair.low),
                Number(pair.high),
                timeoutMs
            );
            response.correlation = {
                low: Number(pair.low),
                high: Number(pair.high)
            };
            return response;
        };

        Client.prototype.requestText = function (
            operation,
            value,
            timeoutMs,
            correlation
        ) {
            var source = String(value);
            var pair = correlation || this.nextCorrelation();
            var response;

            if (timeoutMs === undefined) timeoutMs = DEFAULT_TIMEOUT_MS;
            if (!this.handle) fail("VectorIPC client is not connected", null);
            if (!pair
                || pair.low === undefined
                || pair.high === undefined) {
                fail("invalid VectorIPC correlation pair", null);
            }
            validateTextScalarString(source);
            if (source.length > MAX_PAYLOAD) {
                fail("VectorIPC text payload exceeds adapter limit", null);
            }

            response = requireOK(this._call([
                CMD_TRANSACT_TEXT,
                this.handle,
                timeoutMs,
                operation,
                Number(pair.low),
                Number(pair.high),
                source
            ]), "TRANSACT_TEXT");

            if (response.type !== "text"
                || response.kind !== 2
                || response.operation !== Number(operation)
                || response.correlationLow !== Number(pair.low)
                || response.correlationHigh !== Number(pair.high)) {
                fail("invalid VectorIPC text response metadata", response);
            }

            response.correlation = {
                low: Number(pair.low),
                high: Number(pair.high)
            };
            return response;
        };

        Client.prototype.requestUtf8 = Client.prototype.requestText;

        Client.prototype.requestESON = function (
            operation,
            value,
            timeoutMs,
            correlation,
            codec
        ) {
            var eson = resolveESON(codec);
            var text = eson.stringify(value);
            var response;

            if (text === undefined) {
                fail("VectorIPC ESON request serialized to undefined", null);
            }

            response = this.requestText(
                operation,
                text,
                timeoutMs,
                correlation
            );
            response.value = eson.parse(response.payloadText);
            return response;
        };

        Client.prototype.requestJSON = Client.prototype.requestESON;

        Client.prototype.stageBytes = Client.prototype.stageByteArray;
        Client.prototype.appendBytes = Client.prototype.appendByteArray;
        Client.prototype.request = Client.prototype.requestByteArray;

        Client.prototype.send = function (
            kind,
            flags,
            operation,
            correlationLow,
            correlationHigh,
            timeoutMs
        ) {
            if (timeoutMs === undefined) timeoutMs = DEFAULT_TIMEOUT_MS;
            if (!this.handle) fail("VectorIPC client is not connected", null);
            return requireOK(this._call([
                CMD_SEND,
                this.handle,
                kind,
                flags,
                operation,
                correlationLow,
                correlationHigh,
                timeoutMs
            ]), "SEND");
        };

        Client.prototype.receive = function (timeoutMs) {
            if (timeoutMs === undefined) timeoutMs = DEFAULT_TIMEOUT_MS;
            if (!this.handle) fail("VectorIPC client is not connected", null);
            return requireOK(
                this._call([CMD_RECEIVE, this.handle, timeoutMs]),
                "RECEIVE"
            );
        };

        Client.prototype.close = function () {
            var handle;
            var response;
            if (this.disposed || !this.lib || !this.handle) return null;
            handle = this.handle;
            try {
                response = requireOK(
                    this._call([CMD_CLOSE, handle]),
                    "CLOSE"
                );
                return response;
            } finally {
                this.handle = 0;
            }
        };

        Client.prototype.dispose = function () {
            if (this.disposed) return;
            try {
                try {
                    this.close();
                } catch (ignoreClose) {}
                try {
                    this.lib.unload();
                } catch (ignoreUnload) {}
            } finally {
                this.lib = null;
                this.handle = 0;
                this.disposed = true;
            }
        };

        function validateClient(client) {
            var info = client.info();
            if (info.adapterVersion !== ADAPTER_VERSION
                || info.wireMajor !== WIRE_MAJOR
                || info.packBytes !== PACK_BYTES
                || info.maxPayload !== MAX_PAYLOAD
                || info.maxSessions < 1) {
                try {
                    client.dispose();
                } catch (ignoreDispose) {}
                fail(
                    "VectorIPC ExternalObject contract mismatch: " + info.raw,
                    info
                );
            }
            client.capabilities = info;
            return client;
        }

        function bindLibrary(lib) {
            if (!lib || typeof lib.vipc !== "function") {
                try {
                    if (lib) lib.unload();
                } catch (ignoreUnload) {}
                fail("VectorIPC vipc binding unavailable", null);
            }
            return validateClient(new Client(lib));
        }

        function load(nativeDir, baseName) {
            var oldSearch;
            var lib;
            var specifier;

            nativeDir = String(nativeDir);
            if (!baseName) baseName = "VectorIPCExternalObject";
            specifier = "lib:" + String(baseName);

            oldSearch = ExternalObject.searchFolders;
            ExternalObject.searchFolders = nativeDir + ";" + oldSearch;
            try {
                if (!ExternalObject.search(specifier)) {
                    fail("VectorIPC ExternalObject not found in " + nativeDir, null);
                }
                lib = new ExternalObject(specifier);
            } finally {
                ExternalObject.searchFolders = oldSearch;
            }

            return bindLibrary(lib);
        }

        function loadPath(dllPath) {
            var lib;
            try {
                lib = new ExternalObject("lib:" + String(dllPath));
            } catch (error) {
                fail("VectorIPC ExternalObject load failed: " + String(error), null);
            }
            return bindLibrary(lib);
        }

        function errorResult(error) {
            var response = error && error.vipc ? error.vipc : null;
            return {
                ok: false,
                message: String(
                    error && error.message !== undefined
                        ? error.message
                        : error
                ),
                raw: response && response.raw ? response.raw : null,
                response: response,
                error: error
            };
        }

        function open(dllPath) {
            try {
                return {
                    ok: true,
                    client: loadPath(dllPath)
                };
            } catch (error) {
                return errorResult(error);
            }
        }

        function openFromDirectory(nativeDir, baseName) {
            try {
                return {
                    ok: true,
                    client: load(nativeDir, baseName)
                };
            } catch (error) {
                return errorResult(error);
            }
        }

        function decodeBase64(text) {
            var source = String(text);
            var sourceLength = source.length;
            var padding = 0;
            var outputLength;
            var bytes;
            var outputIndex = 0;
            var i;
            var a;
            var b;
            var c;
            var d;
            var aCode;
            var bCode;
            var cCode;
            var dCode;
            var block;

            if ((sourceLength % 4) !== 0) {
                fail("invalid VectorIPC Base64 length", null);
            }
            if (sourceLength === 0) return [];

            if (source.charCodeAt(sourceLength - 1) === 61) padding++;
            if (source.charCodeAt(sourceLength - 2) === 61) padding++;
            outputLength = (sourceLength / 4) * 3 - padding;
            bytes = new Array(outputLength);

            for (i = 0; i < sourceLength; i += 4) {
                aCode = source.charCodeAt(i);
                bCode = source.charCodeAt(i + 1);
                cCode = source.charCodeAt(i + 2);
                dCode = source.charCodeAt(i + 3);

                a = aCode < 128 ? BASE64_DECODE[aCode] : -1;
                b = bCode < 128 ? BASE64_DECODE[bCode] : -1;
                c = cCode === 61
                    ? -2
                    : (cCode < 128 ? BASE64_DECODE[cCode] : -1);
                d = dCode === 61
                    ? -2
                    : (dCode < 128 ? BASE64_DECODE[dCode] : -1);

                if (a < 0 || b < 0 || c === -1 || d === -1) {
                    fail("invalid VectorIPC Base64 payload", null);
                }
                if (c === -2 && d !== -2) {
                    fail("invalid VectorIPC Base64 padding", null);
                }
                if ((c === -2 || d === -2) && i + 4 !== sourceLength) {
                    fail("invalid VectorIPC Base64 padding position", null);
                }
                if (c === -2 && (b & 15) !== 0) {
                    fail("non-canonical VectorIPC Base64 padding bits", null);
                }
                if (d === -2 && c >= 0 && (c & 3) !== 0) {
                    fail("non-canonical VectorIPC Base64 padding bits", null);
                }

                block = (a << 18) | (b << 12);
                if (c >= 0) block |= c << 6;
                if (d >= 0) block |= d;

                bytes[outputIndex++] = (block >> 16) & 255;
                if (c >= 0) bytes[outputIndex++] = (block >> 8) & 255;
                if (d >= 0) bytes[outputIndex++] = block & 255;
            }

            return bytes;
        }

        function decodeBase64BinaryString(text, codec) {
            return resolveESB64(codec).atob(String(text));
        }

        function decodeBase64Hex(text, codec) {
            return resolveESCHARS(codec).b64ToHex(String(text));
        }

        return {
            version: 5,
            adapterVersion: ADAPTER_VERSION,
            maxPayload: MAX_PAYLOAD,
            supportsText: true,
            supportsESON: true,
            supportsESB64: true,
            supportsESCHARS: true,
            packBytes: PACK_BYTES,
            defaultChunkWords: DEFAULT_CHUNK_WORDS,
            maxChunkWords: MAX_CHUNK_WORDS,
            load: load,
            loadPath: loadPath,
            open: open,
            openFromDirectory: openFromDirectory,
            parseResponse: parseResponse,
            decodeBase64: decodeBase64,
            decodeBase64BinaryString: decodeBase64BinaryString,
            decodeBase64Hex: decodeBase64Hex,
            KIND: {
                REQUEST: 1,
                RESPONSE: 2,
                NOTIFY: 3,
                EVENT: 4,
                CONTROL: 5
            },
            FLAG: {
                NONE: 0,
                ERROR: 1
            },
            Client: Client
        };
    }());
}

if (typeof VectorIPC === "undefined" || VectorIPC === null) {
    VectorIPC = VectorIPCExternalObject;
}