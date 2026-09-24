#include <vectoripc/vipc.hpp>

#include <cstdint>
#include <cstdio>

int main() {
    static_assert(VIPC_VERSION_MAJOR == 0u, "unexpected VectorIPC major version");
    static_assert(VIPC_VERSION_MINOR == 1u, "unexpected VectorIPC minor version");
    static_assert(VIPC_VERSION_PATCH == 1u, "unexpected VectorIPC patch version");
    static_assert(VIPC_ABI_VERSION == 1u, "unexpected VectorIPC ABI");
    static_assert(VIPC_WIRE_HEADER_SIZE == 32u, "unexpected VectorIPC wire");

    vectoripc::Error error;
    vectoripc::Channel channel;
    vectoripc::Server server;

    if (channel.is_open() || server) {
        return 1;
    }
    if (error.status() != VIPC_OK || error.phase() != VIPC_PHASE_NONE) {
        return 1;
    }
    if (vipc_abi_version() != VIPC_ABI_VERSION) {
        return 1;
    }

    std::printf("VectorIPC installed-package consumer: PASS\n");
    return 0;
}