#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "LibMqtt/Core/Types.h"
#include "LibMqtt/Core/Callbacks.h"

namespace mqtt { class async_client; }

namespace libmqtt::pub {

    // Bounded-queue worker pool options.
    // Keep queue_capacity_per_worker small to limit e2e latency by default.
    struct PoolQueueOptions {
        int     workers = 0;          // 0 => use min(8, hw_concurrency)
        size_t  queue_capacity_per_worker = 16;
    };

    // PoolQueuePublisher
    // ------------------
    // Single MQTT connection shared by N worker threads.
    // Enqueue() is blocking and bounded (NO user-space drops).
    // Workers perform synchronous publish; for QoS>=1, Publish() waits for broker ACK.
    // Non-throwing API: methods return Status; exceptions never escape.
    class PoolQueuePublisher {
    public:
        PoolQueuePublisher(std::string brokerUri,
            std::string clientId,
            PoolQueueOptions opt = {});
        ~PoolQueuePublisher();

        PoolQueuePublisher(const PoolQueuePublisher&) = delete;
        PoolQueuePublisher& operator=(const PoolQueuePublisher&) = delete;

        // Connect to broker (blocking until CONNACK or timeout inside).
        Status Connect(const std::optional<ConnectionOptions>& opts = std::nullopt) noexcept;

        // Disconnect; stops workers and drains. Always returns Ok.
        Status Disconnect() noexcept;

        bool IsConnected() const noexcept;

        // Bounded blocking enqueue (no drop). Returns Disconnected if not connected.
        Status Enqueue(std::string_view topic,
            std::string_view payload,
            QoS qos,
            bool retained = false) noexcept;

        // Enqueue (wstring -> UTF-8 via Utility::WstringToString)
        Status Enqueue(const std::wstring& topic,
            const std::wstring& payload,
            QoS qos,
            bool retained = false) noexcept;

        // Drain: waits until all queues are empty AND Paho pending tokens are zero.
        // Returns Timeout if 'timeout' elapses before drain completes.
        Status Flush(std::chrono::milliseconds timeout = std::chrono::seconds(10)) noexcept;

        void SetErrorCallback(ErrorCallback cb) noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace libmqtt::pub
