#pragma once
/**
 * @file PoolZeroPublisher.h
 * @brief Single-connection, zero-queue publisher with synchronous semantics.
 *
 * Semantics:
 *  - QoS0: fire-and-forget (no broker ACK awaited).
 *  - QoS1/2: Publish() blocks until broker ACK (PUBACK/PUBCOMP).
 *  - No internal sleeps; connection gating is event-driven.
 *  - Flush(): no-op (always Ok) since QoS>=1 waits inline; QoS0 has nothing to flush.
 */

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <memory>
#include "LibMqtt/Core/Types.h"
#include "LibMqtt/Core/Callbacks.h"

namespace libmqtt::pub {

    class PoolZeroPublisher {
    public:
        PoolZeroPublisher(std::string broker_uri, std::string client_id);
        ~PoolZeroPublisher();

        PoolZeroPublisher(const PoolZeroPublisher&) = delete;
        PoolZeroPublisher& operator=(const PoolZeroPublisher&) = delete;

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

        // No-op flush, see header banner.
        Status Flush(std::chrono::milliseconds timeout) noexcept;

        void   SetErrorCallback(ErrorCallback cb) noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}  // namespace libmqtt::pub