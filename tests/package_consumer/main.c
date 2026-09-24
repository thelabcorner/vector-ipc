#include <vectoripc/vipc.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    vipc_error error;
    vipc_message message;

    vipc_error_clear(&error);
    if (error.status != VIPC_OK
        || error.phase != VIPC_PHASE_NONE
        || error.platform_code != 0u) {
        return 1;
    }

    message.kind = VIPC_KIND_REQUEST;
    message.flags = VIPC_FLAG_NONE;
    message.operation = VIPC_APP_OPERATION_MIN;
    message.payload_size = 0u;
    message.correlation_id = UINT64_C(1);

    if (VIPC_VERSION_MAJOR != 0u
        || VIPC_VERSION_MINOR != 1u
        || VIPC_VERSION_PATCH != 0u
        || strcmp(VIPC_VERSION_STRING, "0.1.0") != 0
        || vipc_abi_version() != VIPC_ABI_VERSION
        || vipc_protocol_validate(&message) != VIPC_PROTOCOL_OK) {
        return 1;
    }

    printf("VectorIPC installed-package C consumer: PASS\n");
    return 0;
}
