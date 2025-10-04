#pragma once
/**
 * @file PoolQueuePublisher.h
 * @brief Bounded ring + worker pool publisher with backpressure/policy knobs.
 *
 * Semantics:
 *  - Bounded per-worker ring (capacity = queue_capacity_per_worker).
 *  - OverflowPolicy:
 *      Block       -> backpressure producers until space.
 *      DropNewest  -> reject incoming item.
 *      DropOldest  -> overwrite oldest item (LRU-like freshness).
 *  - DisconnectPolicy:
 *      Wait        -> wait for connection; never drop (reliability-first).
 *      Drop        -> discard tasks encountered while disconnected (QoS0 perf).
 *  - Flush(): drains rings and waits for broker ACKs (QoS>=1) via AckTracker.
 */

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include "LibMqtt/Core/Types.h"
#include "LibMqtt/Core/Callbacks.h"

namespace libmqtt::pub {

    struct PoolQueueOptions {
        int    workers = 0;                   ///< 0 => auto (min(hw, 8), at least 1)
        size_t queue_capacity_per_worker = 0; ///< 0 => at least 1, recommended >= 64

        enum class OverflowPolicy { Block, DropNewest, DropOldest };
        OverflowPolicy overflow = OverflowPolicy::Block;

        enum class DisconnectPolicy { Wait, Drop };
        DisconnectPolicy on_disconnect = DisconnectPolicy::Wait;
    };

    class PoolQueuePublisher {
    public:
        PoolQueuePublisher(std::string broker_uri,
            std::string client_id,
            PoolQueueOptions opt);
        ~PoolQueuePublisher();

        PoolQueuePublisher(const PoolQueuePublisher&) = delete;
        PoolQueuePublisher& operator=(const PoolQueuePublisher&) = delete;

        Status Connect(const std::optional<ConnectionOptions>& opts = std::nullopt) noexcept;
        Status Disconnect() noexcept;
        bool   IsConnected() const noexcept;

        Status Enqueue(std::string_view topic,
            std::string_view payload,
            QoS qos,
            bool retained = false) noexcept;

        // Windows convenience (UTF-16 input)
        Status Enqueue(const std::wstring& topic,
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
