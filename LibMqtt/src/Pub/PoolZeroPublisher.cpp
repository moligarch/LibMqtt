#include "LibMqtt/Pub/PoolZeroPublisher.h"

#include <string>
#include <vector>
#include <thread>

#include "mqtt/async_client.h"
#include "LibMqtt/Core/Connect.h"
#include "LibMqtt/Core/Logging.h"
#include "utils.h"

using namespace std::chrono_literals;

namespace libmqtt::pub {

    struct PoolZeroPublisher::Impl : public virtual mqtt::callback {
        mqtt::async_client            client;
        std::atomic<bool>             is_connected{ false };
        CallbackRCU<ErrorCallback>    err_cb;

        explicit Impl(std::string brokerUri, std::string clientId)
            : client(std::move(brokerUri), std::move(clientId))
        {
            client.set_callback(*this);
        }

        // mqtt::callback
        void connected(const std::string&) override {
            is_connected.store(true, std::memory_order_release);
            LIBMQTT_LOG(LogLevel::Info, "PoolZeroPublisher: connected");
        }
        void connection_lost(const std::string& cause) override {
            is_connected.store(false, std::memory_order_release);
            if (auto cb = err_cb.get()) (*cb)(ResultCode::Disconnected, cause);
            LIBMQTT_LOG(LogLevel::Warn, "PoolZeroPublisher: connection lost");
        }
        void message_arrived(mqtt::const_message_ptr) override {
            // publisher doesn't consume messages
        }
        void delivery_complete(mqtt::delivery_token_ptr) override {
            // token waited in Publish() for QoS>=1; nothing to do
        }
    };

    PoolZeroPublisher::PoolZeroPublisher(std::string brokerUri, std::string clientId)
        : impl_(std::make_unique<Impl>(std::move(brokerUri), std::move(clientId))) {
    }

    PoolZeroPublisher::~PoolZeroPublisher() {
        (void)Disconnect();
    }

    Status PoolZeroPublisher::Connect(const std::optional<ConnectionOptions>& opts) noexcept {
        auto st = ConnectBlocking(impl_->client, opts, 5s);
        if (!st.ok()) {
            if (auto cb = impl_->err_cb.get()) (*cb)(st.code, "PoolZeroPublisher: connect failed");
            return st;
        }
        impl_->is_connected.store(impl_->client.is_connected(), std::memory_order_release);
        if (!impl_->client.is_connected()) {
            if (auto cb = impl_->err_cb.get()) (*cb)(ResultCode::ProtocolError, "PoolZeroPublisher: not connected");
            return Status{ ResultCode::ProtocolError };
        }
        return Status{ ResultCode::Ok };
    }

    Status PoolZeroPublisher::Disconnect() noexcept {
        auto st = DisconnectQuiet(impl_->client);
        impl_->is_connected.store(false, std::memory_order_release);
        return st;
    }

    bool PoolZeroPublisher::IsConnected() const noexcept {
        return impl_->client.is_connected();
    }

    Status PoolZeroPublisher::Publish(std::string_view topic,
        std::string_view payload,
        QoS qos,
        bool retained) noexcept
    {
        if (!impl_->client.is_connected()) {
            if (auto cb = impl_->err_cb.get()) (*cb)(ResultCode::Disconnected, "PoolZeroPublisher: not connected");
            return Status{ ResultCode::Disconnected };
        }

        try {
            // Build message without extra copies at callsite.
            auto msg = mqtt::make_message(std::string(topic), payload.data(), payload.size());
            msg->set_qos(static_cast<int>(qos));
            msg->set_retained(retained);

            auto tok = impl_->client.publish(msg);

            if (qos >= QoS::AtLeastOnce) {
                tok->wait(); // broker ACK (PUBACK / PUBCOMP)
            }
            return Status{ ResultCode::Ok };
        }
        catch (const mqtt::exception& e) {
            LIBMQTT_LOG(LogLevel::Error, std::string("PoolZeroPublisher: publish exception: ") + e.what());
            if (auto cb = impl_->err_cb.get()) (*cb)(ResultCode::ProtocolError, e.what());
            return Status{ ResultCode::ProtocolError };
        }
        catch (...) {
            LIBMQTT_LOG(LogLevel::Error, "PoolZeroPublisher: publish unknown exception");
            if (auto cb = impl_->err_cb.get()) (*cb)(ResultCode::Unknown, "unknown publish error");
            return Status{ ResultCode::Unknown };
        }
    }

    Status PoolZeroPublisher::Publish(const std::wstring& topic,
        const std::wstring& payload,
        QoS qos,
        bool retained) noexcept
    {
        const std::string t = Utility::WstringToString(topic);
        const std::string p = Utility::WstringToString(payload);
        if ((t.empty() && !topic.empty()) || (p.empty() && !payload.empty()))
            return Status{ ResultCode::ProtocolError };
        return Publish(t, p, qos, retained);
    }

    Status PoolZeroPublisher::Flush(std::chrono::milliseconds timeout) noexcept {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        try {
            for (;;) {
                auto pend = impl_->client.get_pending_delivery_tokens();
                if (pend.empty()) break;

                // Opportunistically wait on currently pending tokens (bounded).
                // Avoid long waits on every token; poll quickly and yield if needed.
                for (auto& t : pend) {
                    if (!t) continue;
                    // Wait a tiny slice to avoid blocking forever when new tokens appear.
                    t->wait_for(std::chrono::milliseconds(1));
                }

                if (std::chrono::steady_clock::now() >= deadline) {
                    LIBMQTT_LOG(LogLevel::Warn, "PoolZeroPublisher: flush timeout");
                    return Status{ ResultCode::Timeout };
                }
                std::this_thread::yield();
            }
            return Status{ ResultCode::Ok };
        }
        catch (const mqtt::exception& e) {
            LIBMQTT_LOG(LogLevel::Error, std::string("PoolZeroPublisher: flush exception: ") + e.what());
            if (auto cb = impl_->err_cb.get()) (*cb)(ResultCode::ProtocolError, e.what());
            return Status{ ResultCode::ProtocolError };
        }
        catch (...) {
            LIBMQTT_LOG(LogLevel::Error, "PoolZeroPublisher: flush unknown exception");
            if (auto cb = impl_->err_cb.get()) (*cb)(ResultCode::Unknown, "unknown flush error");
            return Status{ ResultCode::Unknown };
        }
    }

    void PoolZeroPublisher::SetErrorCallback(ErrorCallback cb) noexcept {
        impl_->err_cb.set(std::move(cb));
    }

} // namespace libmqtt::pub
