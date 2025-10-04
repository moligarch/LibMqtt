#include "LibMqtt/Sub/Subscriber.h"

#include <string>
#include "mqtt/async_client.h"
#include "LibMqtt/Core/Callbacks.h"
#include "LibMqtt/Core/Connect.h"
#include "LibMqtt/Core/Logging.h"
#include "LibMqtt/Core/Types.h"

using namespace std::chrono_literals;

namespace libmqtt::sub {

    struct Subscriber::Impl : public virtual mqtt::callback {
        mqtt::async_client       client;
        std::atomic<bool>        is_connected_{ false };   // <-- renamed to avoid clash with callback name
        CallbackRCU<MessageCallback> msg_cb;
        CallbackRCU<ErrorCallback>   err_cb;

        explicit Impl(std::string brokerUri, std::string clientId)
            : client(std::move(brokerUri), std::move(clientId))
        {
            client.set_callback(*this);
        }

        // mqtt::callback
        void connected(const std::string&) override {
            is_connected_.store(true, std::memory_order_release);
            LIBMQTT_LOG(LogLevel::Info, "Subscriber: connected");
        }
        void connection_lost(const std::string& cause) override {
            is_connected_.store(false, std::memory_order_release);
            if (auto cb = err_cb.get()) (*cb)(ResultCode::Disconnected, cause);
            LIBMQTT_LOG(LogLevel::Warn, "Subscriber: connection lost");
        }
        void message_arrived(mqtt::const_message_ptr msg) override {
            auto cb = msg_cb.get();
            if (!cb) return;
            // Note: get_topic() returns a const std::string&, get_payload_str() returns by value.
            // Bind to const refs so we can form string_view safely for the duration of this call.
            const std::string& topic = msg->get_topic();
            const std::string& pl = msg->get_payload_str();
            (*cb)(std::string_view{ topic }, std::string_view{ pl });
        }
        void delivery_complete(mqtt::delivery_token_ptr) override {
            // no-op for subscriber
        }
    };

    Subscriber::Subscriber(std::string brokerUri, std::string clientId)
        : impl_(std::make_unique<Impl>(std::move(brokerUri), std::move(clientId))) {
    }

    Subscriber::~Subscriber() { (void)Disconnect(); }

    Status Subscriber::Connect(const std::optional<libmqtt::ConnectionOptions>& opts) noexcept {
        auto st = ConnectBlocking(impl_->client, opts, 5s);
        if (!st.ok()) {
            if (auto cb = impl_->err_cb.get())
                (*cb)(st.code, "Subscriber: connect failed");
            return st;
        }
        // Ensure state flag reflects current reality.
        impl_->is_connected_.store(impl_->client.is_connected(), std::memory_order_release);
        if (!impl_->client.is_connected()) {
            if (auto cb = impl_->err_cb.get())
                (*cb)(ResultCode::ProtocolError, "Subscriber: connected() not reported");
            return { ResultCode::ProtocolError };
        }
        return { ResultCode::Ok };
    }

    Status Subscriber::Disconnect() noexcept {
        auto st = DisconnectQuiet(impl_->client);
        impl_->is_connected_.store(false, std::memory_order_release);
        return st;
    }

    bool Subscriber::IsConnected() const noexcept {
        // Trust Paho’s state (authoritative)
        return impl_->client.is_connected();
    }

    Status Subscriber::Subscribe(const std::string& topic, libmqtt::QoS qos) noexcept {
        try {
            auto tok = impl_->client.subscribe(topic, static_cast<int>(qos));
            tok->wait(); // SUBACK
            LIBMQTT_LOG(LogLevel::Info, "Subscriber: subscribed");
            return { ResultCode::Ok };
        }
        catch (const mqtt::exception& e) {
            if (auto cb = impl_->err_cb.get())
                (*cb)(ResultCode::ProtocolError, e.what());
            LIBMQTT_LOG(LogLevel::Error, std::string("Subscriber: subscribe exception: ") + e.what());
            return { ResultCode::ProtocolError };
        }
        catch (...) {
            if (auto cb = impl_->err_cb.get())
                (*cb)(ResultCode::Unknown, "subscribe unknown error");
            LIBMQTT_LOG(LogLevel::Error, "Subscriber: subscribe unknown error");
            return { ResultCode::Unknown };
        }
    }

    void Subscriber::SetMessageCallback(libmqtt::MessageCallback cb) noexcept {
        impl_->msg_cb.set(std::move(cb));
    }

    void Subscriber::SetErrorCallback(libmqtt::ErrorCallback cb) noexcept {
        impl_->err_cb.set(std::move(cb));
    }

} // namespace libmqtt::sub
