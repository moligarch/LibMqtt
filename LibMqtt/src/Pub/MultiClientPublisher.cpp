#include "LibMqtt/Pub/MultiClientPublisher.h"

#define NOMINMAX
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <memory>

#include "LibMqtt/Core/Types.h"
#include "LibMqtt/Core/Logging.h"
#include "LibMqtt/Core/ConnManager.h"
#include "LibMqtt/Core/AckTracker.h"
#include "LibMqtt/Core/OptionsBuilder.h"
#include "utils.h"

namespace libmqtt::pub {

    struct MultiClientPublisher::Impl {
        std::string                 broker;
        std::string                 idbase;
        MultiClientOptions          opt;

        struct Conn {
            std::mutex                  mu;   // order per connection
            std::unique_ptr<ConnManager> cm;  // owns async_client + connection state
        };

        // Use pointers in a vector (or std::deque<Conn>) to avoid moving a type with std::mutex.
        std::vector<std::unique_ptr<Conn>> conns;

        std::atomic<uint32_t>       rr{ 0 };
        std::optional<ConnectionOptions> saved_opts;
        CallbackRCU<ErrorCallback>  err_cb;

        AckTracker                  ack;     // QoS>=1 flush tracker

        explicit Impl(std::string b, std::string idb, MultiClientOptions o)
            : broker(std::move(b)), idbase(std::move(idb)), opt(o) {
        }
    };

    MultiClientPublisher::MultiClientPublisher(std::string brokerUri,
        std::string clientIdBase,
        MultiClientOptions opt)
        : impl_(std::make_unique<Impl>(std::move(brokerUri), std::move(clientIdBase), opt)) {
    }

    MultiClientPublisher::~MultiClientPublisher() { (void)Disconnect(); }

    Status MultiClientPublisher::Connect(const std::optional<ConnectionOptions>& opts) noexcept {
        impl_->saved_opts = opts;

        int n = impl_->opt.connections;
        if (n <= 0) {
            int hw = (int)std::thread::hardware_concurrency();
            if (hw <= 0) hw = 4;
            n = std::max(1, std::min(8, hw));
        }

        impl_->conns.clear();
        impl_->conns.reserve((size_t)n);

        for (int i = 0; i < n; ++i) {
            std::string cid = impl_->idbase + "-" + std::to_string(i);
            auto c = std::make_unique<Impl::Conn>();
            c->cm = std::make_unique<ConnManager>(impl_->broker, cid, &impl_->err_cb);

            auto st = c->cm->Connect(impl_->saved_opts);
            if (!st.ok()) {
                for (auto& x : impl_->conns) { if (x && x->cm) (void)x->cm->Disconnect(); }
                impl_->conns.clear();
                return st;
            }
            impl_->conns.emplace_back(std::move(c));
        }
        return Status{ ResultCode::Ok };
    }

    Status MultiClientPublisher::Disconnect() noexcept {
        for (auto& c : impl_->conns) {
            if (c && c->cm) (void)c->cm->Disconnect();
        }
        impl_->conns.clear();
        return Status{ ResultCode::Ok };
    }

    bool MultiClientPublisher::IsConnected() const noexcept {
        for (auto& c : impl_->conns) {
            if (c && c->cm && c->cm->latch().is_connected())
                return true;
        }
        return false;
    }

    Status MultiClientPublisher::Publish(std::string_view topic,
        std::string_view payload,
        QoS qos, bool retained) noexcept
    {
        if (impl_->conns.empty()) return Status{ ResultCode::Disconnected };

        const size_t n = impl_->conns.size();
        const uint32_t start = impl_->rr.fetch_add(1, std::memory_order_relaxed);

        auto try_on = [&](size_t idx) -> Status {
            auto& cn = *impl_->conns[idx];
            std::lock_guard<std::mutex> lk(cn.mu);

            // Event-driven: ensure connected before publish (no sleeps)
            cn.cm->latch().wait_connected();

            auto& cli = cn.cm->client();
            try {
                auto msg = mqtt::make_message(std::string(topic),
                    std::string(payload.data(), payload.size()));
                msg->set_qos((int)qos);
                msg->set_retained(retained);

                auto tok = cli.publish(msg);

                if (qos >= QoS::AtLeastOnce) {
                    impl_->ack.inc();
                    try { tok->wait(); }
                    catch (...) { impl_->ack.dec_notify(); throw; }
                    impl_->ack.dec_notify();
                }
                return Status{ ResultCode::Ok };
            }
            catch (const mqtt::exception&) {
                // Likely raced with disconnect; wait for reconnect once and retry once.
                cn.cm->latch().wait_connected();
                try {
                    auto msg2 = mqtt::make_message(std::string(topic),
                        std::string(payload.data(), payload.size()));
                    msg2->set_qos((int)qos);
                    msg2->set_retained(retained);
                    auto tok2 = cli.publish(msg2);
                    if (qos >= QoS::AtLeastOnce) {
                        impl_->ack.inc();
                        try { tok2->wait(); }
                        catch (...) { impl_->ack.dec_notify(); throw; }
                        impl_->ack.dec_notify();
                    }
                    return Status{ ResultCode::Ok };
                }
                catch (...) {
                    return Status{ ResultCode::Disconnected };
                }
            }
            catch (...) {
                return Status{ ResultCode::Unknown };
            }
            };

        // Try selected connection, then failover across the rest.
        for (size_t k = 0; k < n; ++k) {
            size_t idx = (start + k) % n;
            auto st = try_on(idx);
            if (st.ok()) return st;
        }
        if (auto cb = impl_->err_cb.get())
            (*cb)(ResultCode::Disconnected, "publish failed on all connections");
        return Status{ ResultCode::Disconnected };
    }

    Status MultiClientPublisher::Publish(const std::wstring& topic,
        const std::wstring& payload,
        QoS qos, bool retained) noexcept
    {
        const std::string t = Utility::WstringToString(topic);
        const std::string p = Utility::WstringToString(payload);
        if ((t.empty() && !topic.empty()) || (p.empty() && !payload.empty()))
            return Status{ ResultCode::ProtocolError };
        return Publish(t, p, qos, retained);
    }

    Status MultiClientPublisher::Flush(std::chrono::milliseconds timeout) noexcept {
        // QoS0: nothing to wait on; QoS>=1: wait for tracked in-flight == 0
        return impl_->ack.wait_zero(timeout);
    }

    void MultiClientPublisher::SetErrorCallback(ErrorCallback cb) noexcept {
        impl_->err_cb.set(std::move(cb));
    }

} // namespace libmqtt::pub
