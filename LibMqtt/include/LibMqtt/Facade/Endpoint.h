#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <filesystem>

#include "LibMqtt/Core/Types.h"
#include "LibMqtt/Core/Callbacks.h"

// Publishers
#include "LibMqtt/Pub/MultiClientPublisher.h"
#include "LibMqtt/Pub/PoolZeroPublisher.h"
#include "LibMqtt/Pub/PoolQueuePublisher.h"

// Subscriber
#include "LibMqtt/Sub/Subscriber.h"

namespace libmqtt {

    // Which publisher strategy to use
    enum class PublisherKind { MultiClient, PoolZero, PoolQueue };

    // Options for Endpoint façade
    struct EndpointOptions {
        PublisherKind kind{ PublisherKind::MultiClient };
        pub::MultiClientOptions multi{};     // used if kind == MultiClient
        pub::PoolQueueOptions   poolQueue{}; // used if kind == PoolQueue
        std::optional<ConnectionOptions> conn; // TLS/auth/LWT
    };

    // Endpoint
    // --------
    // A convenience façade that composes one Publisher (selected by kind) and a
    // single Subscriber. All APIs are non-throwing (return Status). Reliability
    // first: for QoS>=1, Publish() returns only after broker ACK.
    // Overloads accepting std::wstring convert to UTF-8 via Utility::WstringToString.
    //
    // Notes:
    //  - You can use Endpoint only for publishing, only for subscribing,
    //    or both; just ignore the side you don’t need.
    //  - SetErrorCallback() attaches the same callback to both publisher & subscriber.
    class Endpoint {
    public:
        Endpoint(std::string brokerUri,
            std::string pubClientIdBase,
            std::string subClientId,
            EndpointOptions opts = {});
        ~Endpoint();

        Endpoint(const Endpoint&) = delete;
        Endpoint& operator=(const Endpoint&) = delete;

        // Connect publisher and/or subscriber depending on use.
        // If you don't need subscribe, simply don't call Subscribe().
        Status Connect() noexcept;

        // Disconnect both sides; always returns Ok.
        Status Disconnect() noexcept;

        // Publish (UTF-8)
        Status Publish(std::string_view topic,
            std::string_view payload,
            QoS qos,
            bool retained = false) noexcept;

        // Publish (wstring -> UTF-8 via Utility::WstringToString)
        Status Publish(const std::wstring& topic,
            const std::wstring& payload,
            QoS qos,
            bool retained = false) noexcept;

        // Flush publisher deliveries
        Status Flush(std::chrono::milliseconds timeout = std::chrono::seconds(10)) noexcept;

        // Subscribe (UTF-8)
        Status Subscribe(const std::string& topic, QoS qos) noexcept;

        // Subscribe (wstring -> UTF-8 via Utility::WstringToString)
        Status Subscribe(const std::wstring& topic, QoS qos) noexcept;

        // Install callbacks
        void SetMessageCallback(MessageCallback cb) noexcept; // subscriber only
        void SetErrorCallback(ErrorCallback cb) noexcept;     // both pub+sub

        // Individual connection states
        bool IsPublisherConnected() const noexcept;
        bool IsSubscriberConnected() const noexcept;
        PublisherKind Kind() const noexcept { return kind_; }

    private:
        // Wire error callbacks to sub-components using our RCU
        void WireErrorCallbacks_() noexcept;

    private:
        std::string broker_;
        std::string pubIdBase_;
        std::string subId_;
        EndpointOptions opts_;
        PublisherKind kind_{ PublisherKind::MultiClient };

        // Components (only one publisher is active)
        std::unique_ptr<pub::MultiClientPublisher> multi_;
        std::unique_ptr<pub::PoolZeroPublisher>    p0_;
        std::unique_ptr<pub::PoolQueuePublisher>   pq_;
        std::unique_ptr<sub::Subscriber>           sub_;

        CallbackRCU<ErrorCallback> err_cb_;
    };

} // namespace libmqtt
