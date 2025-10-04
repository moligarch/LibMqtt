#include "LibMqtt/Pub/PoolZeroPublisher.h"

#define NOMINMAX
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>

#include "LibMqtt/Core/Types.h"
#include "LibMqtt/Core/Logging.h"
#include "LibMqtt/Core/ConnManager.h"
#include "LibMqtt/Core/OptionsBuilder.h"
#include "utils.h" // Utility::WstringToString

namespace libmqtt::pub {

    // Zero-queue, single-connection, synchronous publish semantics.
    // Event-driven (no sleeps). Reliability-first: waits for connection and retries once if a race occurs.
    struct PoolZeroPublisher::Impl {
        std::unique_ptr<ConnManager> cm;
        CallbackRCU<ErrorCallback>    err_cb;

        explicit Impl(std::string broker, std::string cid) {
            cm = std::make_unique<ConnManager>(std::move(broker), std::move(cid), &err_cb);
        }
    };

    PoolZeroPublisher::PoolZeroPublisher(std::string brokerUri,
        std::string clientId)
        : impl_(std::make_unique<Impl>(std::move(brokerUri), std::move(clientId))) {
    }

    PoolZeroPublisher::~PoolZeroPublisher() { (void)Disconnect(); }

    Status PoolZeroPublisher::Connect(const std::optional<ConnectionOptions>& opts) noexcept {
        return impl_->cm->Connect(opts);
    }

    Status PoolZeroPublisher::Disconnect() noexcept {
        return impl_->cm->Disconnect();
    }

    bool PoolZeroPublisher::IsConnected() const noexcept {
        return impl_->cm->latch().is_connected();
    }

    Status PoolZeroPublisher::Publish(std::string_view topic,
        std::string_view payload,
        QoS qos,
        bool retained) noexcept
    {
        // Event-driven: wait until connection is up (no sleeps).
        impl_->cm->latch().wait_connected();

        auto& cli = impl_->cm->client();

        auto do_pub = [&](bool wait_ack) -> Status {
            try {
                auto msg = mqtt::make_message(std::string(topic),
                    std::string(payload.data(), payload.size()));
                msg->set_qos(static_cast<int>(qos));
                msg->set_retained(retained);
                auto tok = cli.publish(msg);
                if (wait_ack) tok->wait(); // QoS1/2 sync with broker
                return Status{ ResultCode::Ok };
            }
            catch (const mqtt::exception& e) {
                if (auto cb = impl_->err_cb.get()) (*cb)(ResultCode::ProtocolError, e.what());
                return Status{ ResultCode::Disconnected };
            }
            catch (...) {
                if (auto cb = impl_->err_cb.get()) (*cb)(ResultCode::Unknown, "unknown publish error");
                return Status{ ResultCode::Unknown };
            }
            };

        const bool wait_ack = (qos >= QoS::AtLeastOnce);

        // First try
        if (auto st = do_pub(wait_ack); st.ok()) return st;

        // Race with disconnect: wait for reconnect once and retry.
        impl_->cm->latch().wait_connected();
        return do_pub(wait_ack);
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

    void PoolZeroPublisher::SetErrorCallback(ErrorCallback cb) noexcept {
        impl_->err_cb.set(std::move(cb));
    }

} // namespace libmqtt::pub
