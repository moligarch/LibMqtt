#pragma once
#include <atomic>
#include <optional>
#include <string>
#include <string_view>
#include <chrono>

#include "LibMqtt/Core/Types.h"
#include "LibMqtt/Core/Callbacks.h"

namespace mqtt { class async_client; }

namespace libmqtt::pub {

    // PoolZeroPublisher
    // -----------------
    // Single MQTT connection; no internal queue. Thread-safe: many caller threads
    // may call Publish() concurrently. For QoS >= 1, Publish() blocks until the
    // broker acknowledges (delivery token completes), providing reliability without
    // user-space buffering.
    //
    // Non-throwing: all methods return Status; no exceptions escape the API.
    class PoolZeroPublisher {
    public:
        PoolZeroPublisher(std::string brokerUri, std::string clientId);
        ~PoolZeroPublisher();

        PoolZeroPublisher(const PoolZeroPublisher&) = delete;
        PoolZeroPublisher& operator=(const PoolZeroPublisher&) = delete;

        // Establish the connection (blocking until CONNACK or timeout inside).
        Status Connect(const std::optional<ConnectionOptions>& opts = std::nullopt) noexcept;

        // Disconnect quietly. Always returns Ok.
        Status Disconnect() noexcept;

        // Paho authoritative connection state.
        bool IsConnected() const noexcept;

        // Reliable publish:
        // - QoS0: fire-and-forget.
        // - QoS1/QoS2: waits until broker ACK (token completed), then returns.
        // NOTE: topic/payload are copied into the Paho message; pass string_view
        //       to avoid extra construction at the callsite.
        Status Publish(std::string_view topic,
            std::string_view payload,
            QoS qos,
            bool retained = false) noexcept;

        // Publish (wstring -> UTF-8 via Utility::WstringToString)
        Status Publish(const std::wstring& topic,
            const std::wstring& payload,
            QoS qos,
            bool retained = false) noexcept;

        // Wait for all outstanding deliveries to complete (best-effort).
        // Returns Timeout if not drained by 'timeout'.
        Status Flush(std::chrono::milliseconds timeout = std::chrono::seconds(10)) noexcept;

        // Error callback invoked on connection_lost or other library-level errors.
        void SetErrorCallback(ErrorCallback cb) noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace libmqtt::pub
