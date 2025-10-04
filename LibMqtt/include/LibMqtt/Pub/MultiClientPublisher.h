#pragma once
#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include "LibMqtt/Core/Types.h"
#include "LibMqtt/Core/Callbacks.h"

namespace libmqtt::pub {

    // Options controlling the number of parallel connections
    struct MultiClientOptions {
        int connections = 0; // 0 => use min(8, hw_concurrency)
    };

    // MultiClientPublisher
    // --------------------
    // K separate MQTT connections. Each Publish() selects a connection via
    // lock-free round-robin. QoS>=1 waits on the delivery token (broker ACK).
    //
    // Non-throwing API: all methods return Status; no exceptions escape.
    //
    // Reliability:
    //  - QoS0  : fire-and-forget on the chosen connection
    //  - QoS1/2: publish() blocks until PUBACK/PUBCOMP (broker accepted)
    // Backpressure comes from the synchronous wait on QoS>=1; there is no
    // user-space queue here.
    class MultiClientPublisher {
    public:
        MultiClientPublisher(std::string brokerUri,
            std::string clientIdPrefix,
            MultiClientOptions opt = {});
        ~MultiClientPublisher();

        MultiClientPublisher(const MultiClientPublisher&) = delete;
        MultiClientPublisher& operator=(const MultiClientPublisher&) = delete;

        // Establish all connections (blocking until CONNACK for each).
        Status Connect(const std::optional<ConnectionOptions>& opts = std::nullopt) noexcept;

        // Disconnect all. Always returns Ok.
        Status Disconnect() noexcept;

        // All-connected state (authoritative via Paho)
        bool IsConnected() const noexcept;

        // Publish on one connection selected via RR.
        Status Publish(std::string_view topic,
            std::string_view payload,
            QoS qos,
            bool retained = false) noexcept;

        // Publish (wstring -> UTF-8 via Utility::WstringToString)
        Status Publish(const std::wstring& topic,
            const std::wstring& payload,
            QoS qos,
            bool retained = false) noexcept;

        // Drain all pending deliveries across all connections (best-effort).
        Status Flush(std::chrono::milliseconds timeout = std::chrono::seconds(10)) noexcept;

        // Error callback invoked on connection_lost or protocol errors.
        void SetErrorCallback(ErrorCallback cb) noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace libmqtt::pub
