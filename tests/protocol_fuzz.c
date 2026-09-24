#include "vectoripc/vipc_protocol.h"

#include <stdio.h>
#include <string.h>

#define VALID_CASES 250000u
#define MUTATION_CASES 250000u

static uint64_t g_state = UINT64_C(0x7f4a7c159e3779b9);

static uint64_t next_u64(void) {
    uint64_t x = g_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_state = x;
    return x * UINT64_C(2685821657736338717);
}

static uint32_t next_u32(void) {
    return (uint32_t)(next_u64() >> 32);
}

static int status_is_defined(vipc_protocol_status status) {
    return status >= VIPC_PROTOCOL_OK
        && status <= VIPC_PROTOCOL_PAYLOAD_TOO_LARGE;
}

int main(void) {
    uint32_t i;

    for (i = 0; i < VALID_CASES; ++i) {
        vipc_message source;
        vipc_message decoded;
        uint8_t header[VIPC_WIRE_HEADER_SIZE];
        vipc_protocol_status status;

        source.kind = (uint32_t)(VIPC_KIND_REQUEST + (next_u32() % 5u));
        source.flags = (next_u32() & 1u) ? VIPC_FLAG_ERROR : VIPC_FLAG_NONE;
        source.operation = next_u32();
        source.correlation_id = next_u64();
        source.payload_size = next_u32() % (VIPC_MAX_PAYLOAD_BYTES + 1u);

        status = vipc_protocol_encode(&source, header);
        if (status != VIPC_PROTOCOL_OK) {
            fprintf(stderr, "valid encode failed at case %lu: %s\n",
                (unsigned long)i, vipc_protocol_status_name(status));
            return 1;
        }

        status = vipc_protocol_decode(header, &decoded);
        if (status != VIPC_PROTOCOL_OK
            || decoded.kind != source.kind
            || decoded.flags != source.flags
            || decoded.operation != source.operation
            || decoded.correlation_id != source.correlation_id
            || decoded.payload_size != source.payload_size) {
            fprintf(stderr, "round-trip mismatch at case %lu: %s\n",
                (unsigned long)i, vipc_protocol_status_name(status));
            return 1;
        }
    }

    for (i = 0; i < MUTATION_CASES; ++i) {
        vipc_message source;
        vipc_message decoded;
        vipc_message sentinel;
        uint8_t header[VIPC_WIRE_HEADER_SIZE];
        uint32_t offset;
        uint8_t delta;
        vipc_protocol_status status;

        source.kind = VIPC_KIND_REQUEST;
        source.flags = VIPC_FLAG_NONE;
        source.operation = VIPC_APP_OPERATION_MIN + 1u;
        source.correlation_id = next_u64();
        source.payload_size = next_u32() % 4097u;

        if (vipc_protocol_encode(&source, header) != VIPC_PROTOCOL_OK) {
            fprintf(stderr, "mutation seed encode failed\n");
            return 1;
        }

        offset = next_u32() % VIPC_WIRE_HEADER_SIZE;
        delta = (uint8_t)((next_u32() % 255u) + 1u);
        header[offset] ^= delta;

        memset(&sentinel, 0xa5, sizeof(sentinel));
        decoded = sentinel;
        status = vipc_protocol_decode(header, &decoded);
        if (!status_is_defined(status)) {
            fprintf(stderr, "undefined status at mutation %lu: %lu\n",
                (unsigned long)i, (unsigned long)status);
            return 1;
        }
        if (status != VIPC_PROTOCOL_OK
            && memcmp(&decoded, &sentinel, sizeof(decoded)) != 0) {
            fprintf(stderr,
                "failed decode modified output at mutation %lu: %s\n",
                (unsigned long)i,
                vipc_protocol_status_name(status));
            return 1;
        }
    }

    printf(
        "VectorIPC protocol fuzz: PASS (%lu valid + %lu mutated headers)\n",
        (unsigned long)VALID_CASES,
        (unsigned long)MUTATION_CASES);
    return 0;
}