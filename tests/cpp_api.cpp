#include "vectoripc/vipc.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

constexpr std::uint32_t kTimeoutMs = 2000u;
constexpr std::uint32_t kOperation = VIPC_APP_OPERATION_MIN + 90u;

bool check(bool condition, const char* label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        return false;
    }
    return true;
}

}  // namespace

int main() {
#ifdef _WIN32
    char endpoint[64]{};
    (void)sprintf_s(
        endpoint,
        sizeof(endpoint),
        "cpp-api-%lu",
        static_cast<unsigned long>(GetCurrentProcessId()));

    vectoripc::Server server;
    vectoripc::Error error;
    if (!check(
            server.open(endpoint, &error) == VIPC_OK,
            "server open")) {
        return 1;
    }

    bool server_ok = true;
    std::thread server_thread([&]() {
        vectoripc::Channel accepted;
        vectoripc::Error server_error;
        vectoripc::Message request{};
        std::array<std::uint8_t, 32> payload{};
        std::uint32_t payload_size = 0u;

        server_ok = check(
            server.accept(kTimeoutMs, accepted, &server_error) == VIPC_OK,
            "server accept");
        if (!server_ok) return;

        server_ok = check(
            accepted.receive(
                request,
                payload.data(),
                static_cast<std::uint32_t>(payload.size()),
                payload_size,
                kTimeoutMs,
                &server_error) == VIPC_OK,
            "server receive");
        if (!server_ok) return;

        const std::array<std::uint8_t, 5> expected{
            0x00u, 0x01u, 0xffu, 0x80u, 0x7fu
        };
        server_ok = check(
            request.kind == static_cast<std::uint32_t>(VIPC_KIND_REQUEST)
                && request.operation == kOperation
                && request.correlation_id == UINT64_C(0x1122334455667788)
                && payload_size == expected.size()
                && std::memcmp(
                    payload.data(),
                    expected.data(),
                    expected.size()) == 0,
            "server request identity");
        if (!server_ok) return;

        vectoripc::Message response{};
        response.kind = static_cast<std::uint32_t>(VIPC_KIND_RESPONSE);
        response.flags = VIPC_FLAG_NONE;
        response.operation = request.operation;
        response.correlation_id = request.correlation_id;
        response.payload_size = payload_size;

        server_ok = check(
            accepted.send(
                response,
                payload.data(),
                kTimeoutMs,
                &server_error) == VIPC_OK,
            "server send");
    });

    vectoripc::Channel client;
    if (!check(
            vectoripc::Channel::connect(
                endpoint,
                kTimeoutMs,
                client,
                &error) == VIPC_OK,
            "client connect")) {
        server_thread.join();
        return 1;
    }

    if (!check(client.is_open(), "client open")) {
        server_thread.join();
        return 1;
    }

    /*
     * Move ownership is the principal value of the C++ façade: native plug-in
     * worker code can hand a channel to an owning object/thread without a
     * second destroy path.
     */
    vectoripc::Channel moved = std::move(client);
    if (!check(!client.is_open() && moved.is_open(), "channel move ownership")) {
        server_thread.join();
        return 1;
    }

    const std::array<std::uint8_t, 5> request_payload{
        0x00u, 0x01u, 0xffu, 0x80u, 0x7fu
    };
    vectoripc::Message request{};
    request.kind = static_cast<std::uint32_t>(VIPC_KIND_REQUEST);
    request.flags = VIPC_FLAG_NONE;
    request.operation = kOperation;
    request.correlation_id = UINT64_C(0x1122334455667788);
    request.payload_size =
        static_cast<std::uint32_t>(request_payload.size());

    if (!check(
            moved.send(
                request,
                request_payload.data(),
                kTimeoutMs,
                &error) == VIPC_OK,
            "client send")) {
        server_thread.join();
        return 1;
    }

    vectoripc::Message response{};
    std::array<std::uint8_t, 32> response_payload{};
    std::uint32_t response_size = 0u;
    if (!check(
            moved.receive(
                response,
                response_payload.data(),
                static_cast<std::uint32_t>(response_payload.size()),
                response_size,
                kTimeoutMs,
                &error) == VIPC_OK,
            "client receive")) {
        server_thread.join();
        return 1;
    }

    if (!check(
            response.kind == static_cast<std::uint32_t>(VIPC_KIND_RESPONSE)
                && response.operation == request.operation
                && response.correlation_id == request.correlation_id
                && response_size == request_payload.size()
                && std::memcmp(
                    response_payload.data(),
                    request_payload.data(),
                    request_payload.size()) == 0,
            "client response identity")) {
        server_thread.join();
        return 1;
    }

    server_thread.join();
    if (!server_ok) return 1;

    /*
     * Server move semantics are checked after the accept loop is finished.
     */
    vectoripc::Server moved_server = std::move(server);
    if (!check(!server && static_cast<bool>(moved_server), "server move ownership")) {
        return 1;
    }

    std::printf("VectorIPC C++17 RAII API: PASS\n");
    return 0;
#else
    std::printf("VectorIPC C++17 RAII API: SKIP (Windows transport only)\n");
    return 0;
#endif
}