#include "LibMqtt/Pub/MultiClientPublisher.h"
#define NOMINMAX
#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "mqtt/async_client.h"
#include "LibMqtt/Core/Connect.h"
#include "LibMqtt/Core/Logging.h"
#include "utils.h"

using namespace std::chrono_literals;

namespace libmqtt::pub {

    struct MultiClientPublisher::Impl {
        struct Conn : public virtual mqtt::callback {
            mqtt::async_client     client;
            std::atomic<bool>      is_connected{ false };
            // Non-owning backref to publisher-level error callback holder
            CallbackRCU<ErrorCallback>* err_cb_rcu{ nullptr };

            Conn(std::string broker, std::string clientId, CallbackRCU<ErrorCallback>* cb_rcu)
                : client(std::move(broker), std::move(clientId)), err_cb_rcu(cb_rcu) {
                client.set_callback(*this);
            }

            // mqtt::callback
            void connected(const std::string&) override {
                is_connected.store(true, std::memory_order_release);
                LIBMQTT_LOG(LogLevel::Info, "MultiClient: connected");
            }
            void connection_lost(const std::string& cause) override {
                is_connected.store(false, std::memory_order_release);
                if (err_cb_rcu) {
                    if (auto cb = err_cb_rcu->get()) (*cb)(ResultCode::Disconnected, cause);
                }
                LIBMQTT_LOG(LogLevel::Warn, "MultiClient: connection lost");
            }
            void message_arrived(mqtt::const_message_ptr) override {
                // publisher doesn't consume messages
            }
            void delivery_complete(mqtt::delivery_token_ptr) override {
                // QoS>=1 waits in Publish(); nothing to do here.
            }
        };

        std::string                    broker_;
        std::string                    prefix_;
        int                            n_{ 0 };
        std::vector<std::unique_ptr<Conn>> conns_;
        CallbackRCU<ErrorCallback>     err_cb_;
        std::atomic<uint32_t>          rr_{ 0 };

        explicit Impl(std::string broker, std::string prefix, MultiClientOptions opt)
            : broker_(std::move(broker)), prefix_(std::move(prefix)) {
            int hw = static_cast<int>(std::thread::hardware_concurrency());
            if (hw <= 0) hw = 8;
            n_ = opt.connections > 0 ? opt.connections : std::min(8, hw);
            if (n_ <= 0) n_ = 1;
        }

        // Return index of a connected connection using RR; -1 if none connected
        int pick_connected() noexcept {
            if (conns_.empty()) return -1;
            uint32_t start = rr_.fetch_add(1, std::memory_order_relaxed);
            int N = static_cast<int>(conns_.size());
            int i0 = static_cast<int>(start % static_cast<uint32_t>(N));
            for (int k = 0; k < N; ++k) {
                int idx = (i0 + k) % N;
                if (conns_[idx]->client.is_connected()) return idx;
            }
            return -1;
        }
    };

    MultiClientPublisher::MultiClientPublisher(std::string brokerUri,
        std::string clientIdPrefix,
        MultiClientOptions opt)
        : impl_(std::make_unique<Impl>(std::move(brokerUri), std::move(clientIdPrefix), opt)) {
    }

    MultiClientPublisher::~MultiClientPublisher() {
        (void)Disconnect();
    }

    Status MultiClientPublisher::Connect(const std::optional<ConnectionOptions>& opts) noexcept {
        // Build K connections and connect them one by one
        impl_->conns_.clear();
        impl_->conns_.reserve(static_cast<size_t>(impl_->n_));

        for (int i = 0; i < impl_->n_; ++i) {
            std::string cid = impl_->prefix_ + "-" + std::to_string(i);
            auto c = std::make_unique<Impl::Conn>(impl_->broker_, cid, &impl_->err_cb_);
            impl_->conns_.emplace_back(std::move(c));
        }

        // Connect all; if any fails, disconnect all and report error
        for (auto& c : impl_->conns_) {
            auto st = ConnectBlocking(c->client, opts, 5s);
            if (!st.ok() || !c->client.is_connected()) {
                // clean up already-connected ones
                for (auto& d : impl_->conns_) DisconnectQuiet(d->client);
                LIBMQTT_LOG(LogLevel::Error, "MultiClient: connect failure");
                return st.ok() ? Status{ ResultCode::ProtocolError } : st;
            }
            c->is_connected.store(true, std::memory_order_release);
        }

        return Status{ ResultCode::Ok };
    }

    Status MultiClientPublisher::Disconnect() noexcept {
        for (auto& c : impl_->conns_) {
            (void)DisconnectQuiet(c->client);
            c->is_connected.store(false, std::memory_order_release);
        }
        return Status{ ResultCode::Ok };
    }

    bool MultiClientPublisher::IsConnected() const noexcept {
        if (impl_->conns_.empty()) return false;
        for (auto& c : impl_->conns_) {
            if (!c->client.is_connected()) return false;
        }
        return true;
    }

    Status MultiClientPublisher::Publish(std::string_view topic,
        std::string_view payload,
        QoS qos,
        bool retained) noexcept
    {
        int idx = impl_->pick_connected();
        if (idx < 0) {
            if (auto cb = impl_->err_cb_.get())
                (*cb)(ResultCode::Disconnected, "MultiClient: no active connection");
            return Status{ ResultCode::Disconnected };
        }

        auto& cli = impl_->conns_[idx]->client;

        try {
            auto msg = mqtt::make_message(std::string(topic), payload.data(), payload.size());
            msg->set_qos(static_cast<int>(qos));
            msg->set_retained(retained);

            auto tok = cli.publish(msg);
            if (qos >= QoS::AtLeastOnce) {
                tok->wait(); // Broker ACK (PUBACK/PUBCOMP)
            }
            return Status{ ResultCode::Ok };
        }
        catch (const mqtt::exception& e) {
            LIBMQTT_LOG(LogLevel::Error, std::string("MultiClient: publish exception: ") + e.what());
            if (auto cb = impl_->err_cb_.get())
                (*cb)(ResultCode::ProtocolError, e.what());
            return Status{ ResultCode::ProtocolError };
        }
        catch (...) {
            LIBMQTT_LOG(LogLevel::Error, "MultiClient: publish unknown exception");
            if (auto cb = impl_->err_cb_.get())
                (*cb)(ResultCode::Unknown, "unknown publish error");
            return Status{ ResultCode::Unknown };
        }
    }

    Status MultiClientPublisher::Publish(const std::wstring& topic,
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

    Status MultiClientPublisher::Flush(std::chrono::milliseconds timeout) noexcept {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        try {
            for (;;) {
                bool any_pending = false;
                for (auto& c : impl_->conns_) {
                    auto pend = c->client.get_pending_delivery_tokens();
                    if (!pend.empty()) any_pending = true;
                    for (auto& t : pend) {
                        if (!t) continue;
                        t->wait_for(std::chrono::milliseconds(1));
                    }
                }
                if (!any_pending) return Status{ ResultCode::Ok };
                if (std::chrono::steady_clock::now() >= deadline) {
                    LIBMQTT_LOG(LogLevel::Warn, "MultiClient: flush timeout");
                    return Status{ ResultCode::Timeout };
                }
                std::this_thread::yield();
            }
        }
        catch (const mqtt::exception& e) {
            LIBMQTT_LOG(LogLevel::Error, std::string("MultiClient: flush exception: ") + e.what());
            if (auto cb = impl_->err_cb_.get())
                (*cb)(ResultCode::ProtocolError, e.what());
            return Status{ ResultCode::ProtocolError };
        }
        catch (...) {
            LIBMQTT_LOG(LogLevel::Error, "MultiClient: flush unknown exception");
            if (auto cb = impl_->err_cb_.get())
                (*cb)(ResultCode::Unknown, "unknown flush error");
            return Status{ ResultCode::Unknown };
        }
    }

    void MultiClientPublisher::SetErrorCallback(ErrorCallback cb) noexcept {
        impl_->err_cb_.set(std::move(cb));
    }

} // namespace libmqtt::pub
