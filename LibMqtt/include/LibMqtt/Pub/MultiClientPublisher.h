#pragma once
/**
 * @file MultiClientPublisher.h
 * @brief Queue-less publisher that load-balances across N connections.
 *
 * Semantics:
 *  - No internal queue; publish selects a connection round-robin.
 *  - On transient disconnect at publish, waits for reconnect once and retries once.
 *  - QoS0: fire-and-forget.
 *  - QoS1/2: Publish() blocks for broker ACK; Flush() waits for any interleaved acks.
 */

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <memory>
#include "LibMqtt/Core/Types.h"
#include "LibMqtt/Core/Callbacks.h"

namespace libmqtt::pub {

    struct MultiClientOptions {
        int connections = 0;  ///< 0 => auto (min(hw, 8), at least 1)
    };

    class MultiClientPublisher {
    public:
        MultiClientPublisher(std::string broker_uri,
            std::string client_id_base,
            MultiClientOptions opt);
        ~MultiClientPublisher();

        MultiClientPublisher(const MultiClientPublisher&) = delete;
        MultiClientPublisher& operator=(const MultiClientPublisher&) = delete;

        Status Connect(const std::optional<ConnectionOptions>& opts = std::nullopt) noexcept;
        Status Disconnect() noexcept;
        bool   IsConnected() const noexcept;

        Status Publish(std::string_view topic,
            std::string_view payload,
            QoS qos,
            bool retained = false) noexcept;

        // Windows convenience (UTF-16 input)
        Status Publish(const std::wstring& topic,
            const std::wstring& payload,
            QoS qos,
            bool retained = false) noexcept;

        Status Flush(std::chrono::milliseconds timeout) noexcept;

        void   SetErrorCallback(ErrorCallback cb) noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}  // namespace libmqtt::pub