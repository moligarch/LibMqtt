#pragma once
#include <atomic>
#include <optional>
#include <string>
#include "LibMqtt/Core/Types.h"
#include "LibMqtt/Core/Callbacks.h"

namespace mqtt { class async_client; class const_message; class callback; }

namespace libmqtt::sub {

    // Single-connection subscriber with reliable SUBACK and lock-free hot-path callbacks.
    // Non-throwing API: all functions return Status and never throw.
    class Subscriber /*final*/ {
    public:
        Subscriber(std::string brokerUri, std::string clientId);
        ~Subscriber();

        Subscriber(const Subscriber&) = delete;
        Subscriber& operator=(const Subscriber&) = delete;

        // Blocks until CONNACK (or timeout inside ConnectBlocking)
        Status Connect(const std::optional<libmqtt::ConnectionOptions>& opts = std::nullopt) noexcept;
        Status Disconnect() noexcept;

        bool IsConnected() const noexcept;

        // Returns after SUBACK
        Status Subscribe(const std::string& topic, libmqtt::QoS qos) noexcept;

        void SetMessageCallback(libmqtt::MessageCallback cb) noexcept;
        void SetErrorCallback(libmqtt::ErrorCallback cb) noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace libmqtt::sub
