#ifndef VECTORIPC_VIPC_HPP
#define VECTORIPC_VIPC_HPP

#include "vipc.h"

#include <cstdint>
#include <utility>

namespace vectoripc {

using Status = vipc_status;
using Phase = vipc_phase;
using Message = vipc_message;

/*
 * Non-throwing diagnostic storage for the C++ façade.
 *
 * The wrapper never turns transport failures into C++ exceptions. This keeps it
 * suitable for Adobe plug-in worker threads and lets products choose their own
 * exception/error policy above VectorIPC.
 */
class Error final {
public:
    Error() noexcept {
        vipc_error_clear(&value_);
    }

    [[nodiscard]] Status status() const noexcept {
        return value_.status;
    }

    [[nodiscard]] Phase phase() const noexcept {
        return value_.phase;
    }

    [[nodiscard]] std::uint32_t platform_code() const noexcept {
        return value_.platform_code;
    }

    [[nodiscard]] const vipc_error& native() const noexcept {
        return value_;
    }

private:
    friend class Channel;
    friend class Server;

    vipc_error* output() noexcept {
        vipc_error_clear(&value_);
        return &value_;
    }

    vipc_error value_{};
};

class Channel final {
public:
    Channel() noexcept = default;

    explicit Channel(vipc_channel* native) noexcept
        : native_(native) {}

    ~Channel() noexcept {
        reset();
    }

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    Channel(Channel&& other) noexcept
        : native_(other.release()) {}

    Channel& operator=(Channel&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return is_open();
    }

    [[nodiscard]] bool is_open() const noexcept {
        return native_ != nullptr && vipc_channel_is_open(native_) != 0;
    }

    [[nodiscard]] std::uint32_t peer_pid() const noexcept {
        return native_ ? vipc_channel_peer_pid(native_) : 0u;
    }

    [[nodiscard]] std::uint32_t peer_session_id() const noexcept {
        return native_ ? vipc_channel_peer_session_id(native_) : 0u;
    }

    [[nodiscard]] vipc_channel* native_handle() const noexcept {
        return native_;
    }

    vipc_channel* release() noexcept {
        vipc_channel* result = native_;
        native_ = nullptr;
        return result;
    }

    void reset(vipc_channel* replacement = nullptr) noexcept {
        if (native_) {
            vipc_channel_destroy(native_);
        }
        native_ = replacement;
    }

    /*
     * Connect into a temporary native handle first. A failed connect leaves the
     * existing Channel untouched; success atomically replaces its ownership.
     */
    static Status connect(
        const char* endpoint,
        std::uint32_t timeout_ms,
        Channel& out,
        Error* error = nullptr) noexcept {
        vipc_channel* native = nullptr;
        const Status status = vipc_client_connect(
            endpoint,
            timeout_ms,
            &native,
            error ? error->output() : nullptr);
        if (status == VIPC_OK) {
            out.reset(native);
        }
        return status;
    }

    Status send(
        const Message& message,
        const void* payload,
        std::uint32_t timeout_ms,
        Error* error = nullptr) noexcept {
        return vipc_channel_send(
            native_,
            &message,
            payload,
            timeout_ms,
            error ? error->output() : nullptr);
    }

    Status receive(
        Message& message,
        void* payload,
        std::uint32_t payload_capacity,
        std::uint32_t& payload_size,
        std::uint32_t timeout_ms,
        Error* error = nullptr) noexcept {
        return vipc_channel_receive(
            native_,
            &message,
            payload,
            payload_capacity,
            &payload_size,
            timeout_ms,
            error ? error->output() : nullptr);
    }

private:
    vipc_channel* native_ = nullptr;
};

class Server final {
public:
    Server() noexcept = default;

    explicit Server(vipc_server* native) noexcept
        : native_(native) {}

    ~Server() noexcept {
        reset();
    }

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    Server(Server&& other) noexcept
        : native_(other.release()) {}

    Server& operator=(Server&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return native_ != nullptr;
    }

    [[nodiscard]] vipc_server* native_handle() const noexcept {
        return native_;
    }

    vipc_server* release() noexcept {
        vipc_server* result = native_;
        native_ = nullptr;
        return result;
    }

    void reset(vipc_server* replacement = nullptr) noexcept {
        if (native_) {
            vipc_server_destroy(native_);
        }
        native_ = replacement;
    }

    /*
     * Like Channel::connect(), a failed open leaves the existing server intact.
     */
    Status open(
        const char* endpoint,
        Error* error = nullptr) noexcept {
        vipc_server* native = nullptr;
        const Status status = vipc_server_create(
            endpoint,
            &native,
            error ? error->output() : nullptr);
        if (status == VIPC_OK) {
            reset(native);
        }
        return status;
    }

    Status accept(
        std::uint32_t timeout_ms,
        Channel& out,
        Error* error = nullptr) noexcept {
        vipc_channel* native = nullptr;
        const Status status = vipc_server_accept(
            native_,
            timeout_ms,
            &native,
            error ? error->output() : nullptr);
        if (status == VIPC_OK) {
            out.reset(native);
        }
        return status;
    }

private:
    vipc_server* native_ = nullptr;
};

}  // namespace vectoripc

#endif