#pragma once
#include <chrono>
#include <optional>
#include "LibMqtt/Core/Types.h"

// Forward-declare Paho type to avoid exposing it in public headers
namespace mqtt { class async_client; }

namespace libmqtt {

    // Non-throwing: returns Status; never throws.
    // timeout covers CONNECT handshake; automatic reconnect is disabled for determinism.
    Status ConnectBlocking(mqtt::async_client& client,
        const std::optional<ConnectionOptions>& opts,
        std::chrono::milliseconds timeout) noexcept;

    // Quietly disconnect (no-throw). Always returns Ok.
    Status DisconnectQuiet(mqtt::async_client& client) noexcept;

} // namespace libmqtt