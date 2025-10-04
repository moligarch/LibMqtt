#include "LibMqtt/Facade/Endpoint.h"
#include <vector>
#include <thread>
#include <codecvt> // fallback if Utility not present (not used here)

#include "mqtt/async_client.h"           // not exposed; compile linkage only
#include "LibMqtt/Core/Logging.h"

// bring in your converter
#include "utils.h" // declares Utility::WstringToString(const std::wstring&)

using namespace std::chrono_literals;

namespace libmqtt {

    // --- ctor/dtor --------------------------------------------------------------
    Endpoint::Endpoint(std::string brokerUri,
        std::string pubClientIdBase,
        std::string subClientId,
        EndpointOptions opts)
        : broker_(std::move(brokerUri)),
        pubIdBase_(std::move(pubClientIdBase)),
        subId_(std::move(subClientId)),
        opts_(std::move(opts)),
        kind_(opts_.kind)
    {
        // Create selected publisher (not connected yet)
        switch (kind_) {
        case PublisherKind::MultiClient:
            multi_ = std::make_unique<pub::MultiClientPublisher>(broker_, pubIdBase_, opts_.multi);
            break;
        case PublisherKind::PoolZero:
            p0_ = std::make_unique<pub::PoolZeroPublisher>(broker_, pubIdBase_);
            break;
        case PublisherKind::PoolQueue:
            pq_ = std::make_unique<pub::PoolQueuePublisher>(broker_, pubIdBase_, opts_.poolQueue);
            break;
        }
        // Always create subscriber
        sub_ = std::make_unique<sub::Subscriber>(broker_, subId_);
    }

    Endpoint::~Endpoint() {
        (void)Disconnect();
    }

    // --- error callback wiring --------------------------------------------------
    void Endpoint::WireErrorCallbacks_() noexcept {
        auto forward = [this](ResultCode code, std::string_view what) noexcept {
            if (auto cb = err_cb_.get()) (*cb)(code, what);
            };
        if (multi_) multi_->SetErrorCallback(forward);
        if (p0_)    p0_->SetErrorCallback(forward);
        if (pq_)    pq_->SetErrorCallback(forward);
        if (sub_)   sub_->SetErrorCallback(forward);
    }

    // --- connect/disconnect -----------------------------------------------------
    Status Endpoint::Connect() noexcept {
        WireErrorCallbacks_();

        // Connect publisher (if created)
        Status pst{ ResultCode::Ok };
        if (multi_) pst = multi_->Connect(opts_.conn);
        else if (p0_)   pst = p0_->Connect(opts_.conn);
        else if (pq_)   pst = pq_->Connect(opts_.conn);
        if (!pst.ok()) return pst;

        // Connect subscriber
        auto sst = sub_->Connect(opts_.conn);
        if (!sst.ok()) return sst;

        return Status{ ResultCode::Ok };
    }

    Status Endpoint::Disconnect() noexcept {
        if (multi_) (void)multi_->Disconnect();
        if (p0_)    (void)p0_->Disconnect();
        if (pq_)    (void)pq_->Disconnect();
        if (sub_)   (void)sub_->Disconnect();
        return Status{ ResultCode::Ok };
    }

    // --- publish ---------------------------------------------------------------
    Status Endpoint::Publish(std::string_view topic,
        std::string_view payload,
        QoS qos,
        bool retained) noexcept
    {
        if (multi_) return multi_->Publish(topic, payload, qos, retained);
        if (p0_)    return p0_->Publish(topic, payload, qos, retained);
        if (pq_)    return pq_->Enqueue(topic, payload, qos, retained);
        return Status{ ResultCode::Unknown };
    }

    Status Endpoint::Publish(const std::wstring& topic,
        const std::wstring& payload,
        QoS qos,
        bool retained) noexcept
    {
        if (multi_) return multi_->Publish(topic, payload, qos, retained);
        if (p0_)    return p0_->Publish(topic, payload, qos, retained);
        if (pq_)    return pq_->Enqueue(topic, payload, qos, retained);
        return Status{ ResultCode::Unknown };
    }

    // --- flush ------------------------------------------------------------------
    Status Endpoint::Flush(std::chrono::milliseconds timeout) noexcept {
        if (multi_) return multi_->Flush(timeout);
        if (p0_)    return p0_->Flush(timeout);
        if (pq_)    return pq_->Flush(timeout);
        return Status{ ResultCode::Ok };
    }

    // --- subscribe --------------------------------------------------------------
    Status Endpoint::Subscribe(const std::string& topic, QoS qos) noexcept {
        return sub_ ? sub_->Subscribe(topic, qos) : Status{ ResultCode::Unknown };
    }
    Status Endpoint::Subscribe(const std::wstring& topic, QoS qos) noexcept {
        const std::string t = Utility::WstringToString(topic);
        if (t.empty() && !topic.empty()) return Status{ ResultCode::ProtocolError };
        return Subscribe(t, qos);
    }

    // --- callbacks --------------------------------------------------------------
    void Endpoint::SetMessageCallback(MessageCallback cb) noexcept {
        if (sub_) sub_->SetMessageCallback(std::move(cb));
    }
    void Endpoint::SetErrorCallback(ErrorCallback cb) noexcept {
        err_cb_.set(std::move(cb));
        WireErrorCallbacks_();
    }

    // --- states -----------------------------------------------------------------
    bool Endpoint::IsPublisherConnected() const noexcept {
        if (multi_) return multi_->IsConnected();
        if (p0_)    return p0_->IsConnected();
        if (pq_)    return pq_->IsConnected();
        return false;
    }
    bool Endpoint::IsSubscriberConnected() const noexcept {
        return sub_ ? sub_->IsConnected() : false;
    }

} // namespace libmqtt
