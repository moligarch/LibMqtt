#include "LibMqtt/Pub/PoolQueuePublisher.h"
#define NOMINMAX
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "mqtt/async_client.h"
#include "LibMqtt/Core/Connect.h"
#include "LibMqtt/Core/Logging.h"
#include "utils.h"

using namespace std::chrono_literals;

namespace libmqtt::pub {

    struct PoolQueuePublisher::Impl : public virtual mqtt::callback {
        // ----- Shared Paho client -----
        mqtt::async_client         client;
        std::atomic<bool>          is_connected{ false };

        // ----- Error callback (RCU) -----
        CallbackRCU<ErrorCallback> err_cb;

        // ----- Config / state -----
        const int                  workers;
        const size_t               cap;
        std::atomic<uint32_t>      rr{ 0 };
        std::atomic<bool>          stopping{ false };

        // ----- Task model -----
        struct Task {
            std::string topic;
            std::string payload;
            QoS         qos{ QoS::AtLeastOnce };
            bool        retained{ false };
        };

        // ----- Bounded ring queue (SPMC per worker) -----
        struct Ring {
            std::vector<Task>      buf;
            size_t                 head{ 0 }, tail{ 0 }, size{ 0 };
            std::mutex             m;
            std::condition_variable cv_not_full;
            std::condition_variable cv_not_empty;

            explicit Ring(size_t cap) : buf(cap) {}

            void push(Task&& t, std::atomic<bool>& stopping) {
                std::unique_lock<std::mutex> lk(m);
                cv_not_full.wait(lk, [&] { return size < buf.size() || stopping.load(std::memory_order_acquire); });
                if (stopping.load(std::memory_order_acquire)) return;
                buf[tail] = std::move(t);
                tail = (tail + 1) % buf.size();
                ++size;
                lk.unlock();
                cv_not_empty.notify_one();
            }

            bool pop(Task& out, std::atomic<bool>& stopping) {
                std::unique_lock<std::mutex> lk(m);
                cv_not_empty.wait(lk, [&] { return size > 0 || stopping.load(std::memory_order_acquire); });
                if (size == 0) return false; // stopping & empty
                out = std::move(buf[head]);
                head = (head + 1) % buf.size();
                --size;
                lk.unlock();
                cv_not_full.notify_one();
                return true;
            }

            // NOTE: non-const to avoid locking a const mutex (fixes MSVC C2665).
            bool empty() noexcept {
                std::lock_guard<std::mutex> lk(m);
                return size == 0;
            }
        };

        std::vector<std::unique_ptr<Ring>>  rings;
        std::vector<std::thread>            threads;

        Impl(std::string broker, std::string cid, PoolQueueOptions opt)
            : client(std::move(broker), std::move(cid)),
            workers([&] {
            int hw = static_cast<int>(std::thread::hardware_concurrency());
            if (hw <= 0) hw = 8;
            int w = (opt.workers > 0) ? opt.workers : std::min(8, hw);
            return std::max(1, w);
                }()),
            cap(std::max<size_t>(1, opt.queue_capacity_per_worker))
        {
            client.set_callback(*this);
            rings.reserve(static_cast<size_t>(workers));
            for (int i = 0; i < workers; ++i) rings.emplace_back(std::make_unique<Ring>(cap));
        }

        // ----- mqtt::callback -----
        void connected(const std::string&) override {
            is_connected.store(true, std::memory_order_release);
            LIBMQTT_LOG(LogLevel::Info, "PoolQueuePublisher: connected");
        }
        void connection_lost(const std::string& cause) override {
            is_connected.store(false, std::memory_order_release);
            if (auto cb = err_cb.get()) (*cb)(ResultCode::Disconnected, cause);
            LIBMQTT_LOG(LogLevel::Warn, "PoolQueuePublisher: connection lost");
        }
        void message_arrived(mqtt::const_message_ptr) override {}
        void delivery_complete(mqtt::delivery_token_ptr) override {}

        // Worker loop bound to a specific ring index
        void worker_loop(int ring_index) noexcept {
            Task t;
            auto& q = *rings[static_cast<size_t>(ring_index)];
            for (;;) {
                if (!q.pop(t, stopping)) break;              // stopped & empty -> exit
                if (!client.is_connected()) {                // if we lost connection, surface and back off
                    if (auto cb = err_cb.get()) (*cb)(ResultCode::Disconnected, "PoolQueuePublisher: lost connection");
                    std::this_thread::sleep_for(1ms);
                    continue;
                }
                try {
                    auto msg = mqtt::make_message(t.topic, t.payload);
                    msg->set_qos(static_cast<int>(t.qos));
                    msg->set_retained(t.retained);
                    auto tok = client.publish(msg);
                    if (t.qos >= QoS::AtLeastOnce) tok->wait(); // broker ACK
                }
                catch (const mqtt::exception& e) {
                    LIBMQTT_LOG(LogLevel::Error, std::string("PoolQueuePublisher worker: publish exception: ") + e.what());
                    if (auto cb = err_cb.get()) (*cb)(ResultCode::ProtocolError, e.what());
                }
                catch (...) {
                    LIBMQTT_LOG(LogLevel::Error, "PoolQueuePublisher worker: unknown publish error");
                    if (auto cb = err_cb.get()) (*cb)(ResultCode::Unknown, "unknown publish error");
                }
            }
        }
    };

    PoolQueuePublisher::PoolQueuePublisher(std::string brokerUri,
        std::string clientId,
        PoolQueueOptions opt)
        : impl_(std::make_unique<Impl>(std::move(brokerUri), std::move(clientId), opt)) {
    }

    PoolQueuePublisher::~PoolQueuePublisher() {
        (void)Disconnect();
    }

    Status PoolQueuePublisher::Connect(const std::optional<ConnectionOptions>& opts) noexcept {
        auto st = ConnectBlocking(impl_->client, opts, 5s);
        if (!st.ok() || !impl_->client.is_connected()) {
            if (auto cb = impl_->err_cb.get()) (*cb)(st.ok() ? ResultCode::ProtocolError : st.code,
                "PoolQueuePublisher: connect failed");
            return st.ok() ? Status{ ResultCode::ProtocolError } : st;
        }
        impl_->is_connected.store(true, std::memory_order_release);

        // Spawn workers, one per ring. Each is bound to a fixed ring index.
        impl_->stopping.store(false, std::memory_order_release);
        impl_->threads.reserve(static_cast<size_t>(impl_->workers));
        for (int i = 0; i < impl_->workers; ++i) {
            impl_->threads.emplace_back([impl = impl_.get(), i]() noexcept { impl->worker_loop(i); });
        }

        return Status{ ResultCode::Ok };
    }

    Status PoolQueuePublisher::Disconnect() noexcept {
        impl_->stopping.store(true, std::memory_order_release);

        // Wake all waiting producers/consumers
        for (auto& r : impl_->rings) {
            r->cv_not_empty.notify_all();
            r->cv_not_full.notify_all();
        }

        for (auto& t : impl_->threads) if (t.joinable()) t.join();
        impl_->threads.clear();

        (void)DisconnectQuiet(impl_->client);
        impl_->is_connected.store(false, std::memory_order_release);
        return Status{ ResultCode::Ok };
    }

    bool PoolQueuePublisher::IsConnected() const noexcept {
        return impl_->client.is_connected();
    }

    Status PoolQueuePublisher::Enqueue(std::string_view topic,
        std::string_view payload,
        QoS qos,
        bool retained) noexcept
    {
        if (!impl_->client.is_connected()) {
            if (auto cb = impl_->err_cb.get()) (*cb)(ResultCode::Disconnected, "PoolQueuePublisher: not connected");
            return Status{ ResultCode::Disconnected };
        }

        // Choose a ring via lock-free RR to spread producers across workers.
        const uint32_t id = impl_->rr.fetch_add(1, std::memory_order_relaxed);
        auto& q = *impl_->rings[static_cast<size_t>(id % static_cast<uint32_t>(impl_->rings.size()))];

        Impl::Task t;
        t.topic.assign(topic.data(), topic.size());
        t.payload.assign(payload.data(), payload.size());
        t.qos = qos;
        t.retained = retained;

        // Blocking bounded push. If stopping is set during wait, this returns early.
        q.push(std::move(t), impl_->stopping);
        return impl_->stopping.load(std::memory_order_acquire) ? Status{ ResultCode::Cancelled }
        : Status{ ResultCode::Ok };
    }

    Status PoolQueuePublisher::Enqueue(const std::wstring& topic,
        const std::wstring& payload,
        QoS qos,
        bool retained) noexcept
    {
        const std::string t = Utility::WstringToString(topic);
        const std::string p = Utility::WstringToString(payload);
        if ((t.empty() && !topic.empty()) || (p.empty() && !payload.empty()))
            return Status{ ResultCode::ProtocolError };
        return Enqueue(t, p, qos, retained);
    }


    Status PoolQueuePublisher::Flush(std::chrono::milliseconds timeout) noexcept {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        try {
            for (;;) {
                bool all_empty = true;
                for (auto& q : impl_->rings) {
                    if (!q->empty()) { all_empty = false; break; }
                }
                bool any_pending = false;
                auto pend = impl_->client.get_pending_delivery_tokens();
                if (!pend.empty()) any_pending = true;
                for (auto& t : pend) {
                    if (!t) continue;
                    t->wait_for(std::chrono::milliseconds(1));
                }

                if (all_empty && !any_pending) return Status{ ResultCode::Ok };
                if (std::chrono::steady_clock::now() >= deadline) {
                    LIBMQTT_LOG(LogLevel::Warn, "PoolQueuePublisher: flush timeout");
                    return Status{ ResultCode::Timeout };
                }
                std::this_thread::sleep_for(1ms);
            }
        }
        catch (const mqtt::exception& e) {
            LIBMQTT_LOG(LogLevel::Error, std::string("PoolQueuePublisher: flush exception: ") + e.what());
            if (auto cb = impl_->err_cb.get()) (*cb)(ResultCode::ProtocolError, e.what());
            return Status{ ResultCode::ProtocolError };
        }
        catch (...) {
            LIBMQTT_LOG(LogLevel::Error, "PoolQueuePublisher: flush unknown exception");
            if (auto cb = impl_->err_cb.get()) (*cb)(ResultCode::Unknown, "unknown flush error");
            return Status{ ResultCode::Unknown };
        }
    }

    void PoolQueuePublisher::SetErrorCallback(ErrorCallback cb) noexcept {
        impl_->err_cb.set(std::move(cb));
    }

} // namespace libmqtt::pub
