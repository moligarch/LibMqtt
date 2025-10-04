#include "LibMqtt/Pub/PoolQueuePublisher.h"

#define NOMINMAX
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "LibMqtt/Core/Types.h"
#include "LibMqtt/Core/Logging.h"
#include "LibMqtt/Core/ConnManager.h"
#include "LibMqtt/Core/ConnectedLatch.h"
#include "LibMqtt/Core/AckTracker.h"
#include "utils.h"

namespace libmqtt::pub {

    static inline bool is_block(PoolQueueOptions::OverflowPolicy p) noexcept {
        using P = PoolQueueOptions::OverflowPolicy; return p == P::Block;
    }
    static inline bool is_drop_newest(PoolQueueOptions::OverflowPolicy p) noexcept {
        using P = PoolQueueOptions::OverflowPolicy; return p == P::DropNewest;
    }
    static inline bool is_drop_oldest(PoolQueueOptions::OverflowPolicy p) noexcept {
        using P = PoolQueueOptions::OverflowPolicy; return p == P::DropOldest;
    }
    static inline bool drop_on_disc(PoolQueueOptions::DisconnectPolicy p) noexcept {
        using D = PoolQueueOptions::DisconnectPolicy; return p == D::Drop;
    }
    static inline bool wait_on_disc(PoolQueueOptions::DisconnectPolicy p) noexcept {
        using D = PoolQueueOptions::DisconnectPolicy; return p == D::Wait;
    }

    struct PoolQueuePublisher::Impl {
        std::unique_ptr<ConnManager>   cm;     // async_client + connection events
        CallbackRCU<ErrorCallback>     err_cb;

        const int                      workers;
        const size_t                   cap;
        const PoolQueueOptions         opts;

        std::atomic<bool>              stopping{ false };
        std::atomic<uint32_t>          rr{ 0 };

        // ACK tracker so Flush() waits for broker acks on QoS>=1 even if rings are empty
        AckTracker                     ack;

        struct Task {
            std::string topic;
            std::string payload;
            QoS         qos{ QoS::AtLeastOnce };
            bool        retained{ false };
        };

        struct Ring {
            std::vector<Task> buf;
            size_t head{ 0 }, tail{ 0 }, size{ 0 };
            std::mutex m;
            std::condition_variable cv_not_full, cv_not_empty;

            explicit Ring(size_t capacity) : buf(std::max<size_t>(1, capacity)) {}

            bool full_()  const noexcept { return size == buf.size(); }
            bool empty_() const noexcept { return size == 0; }

            bool push(Task&& t, const PoolQueueOptions& opts, std::atomic<bool>& stopping) {
                std::unique_lock<std::mutex> lk(m);
                if (is_block(opts.overflow)) {
                    cv_not_full.wait(lk, [&] { return !full_() || stopping.load(std::memory_order_acquire); });
                    if (stopping.load(std::memory_order_acquire)) return false;
                    buf[tail] = std::move(t);
                    tail = (tail + 1) % buf.size(); ++size;
                    lk.unlock(); cv_not_empty.notify_one();
                    return true;
                }
                if (full_()) {
                    if (is_drop_newest(opts.overflow)) {
                        return false; // reject incoming
                    }
                    // DropOldest: overwrite head
                    buf[head] = std::move(t);
                    head = (head + 1) % buf.size();
                    lk.unlock(); cv_not_empty.notify_one();
                    return true;
                }
                buf[tail] = std::move(t);
                tail = (tail + 1) % buf.size(); ++size;
                lk.unlock(); cv_not_empty.notify_one();
                return true;
            }

            void wait_not_empty(std::unique_lock<std::mutex>& lk, std::atomic<bool>& stopping) {
                cv_not_empty.wait(lk, [&] { return !empty_() || stopping.load(std::memory_order_acquire); });
            }

            Task pop_unlocked() {
                Task out = std::move(buf[head]);
                head = (head + 1) % buf.size();
                --size;
                return out;
            }

            void notify_not_full() { cv_not_full.notify_one(); }

            void wait_empty(std::unique_lock<std::mutex>& lk, std::atomic<bool>& stopping) {
                cv_not_full.wait(lk, [&] { return empty_() || stopping.load(std::memory_order_acquire); });
            }

            bool empty() {
                std::lock_guard<std::mutex> lk(m);
                return empty_();
            }
        };

        std::vector<std::unique_ptr<Ring>> rings;
        std::vector<std::thread>           threads;

        Impl(std::string broker, std::string cid, PoolQueueOptions o)
            : workers([&] {
            int hw = (int)std::thread::hardware_concurrency(); if (hw <= 0) hw = 8;
            int w = (o.workers > 0) ? o.workers : std::min(8, hw);
            return std::max(1, w);
                }()),
            cap(std::max<size_t>(1, o.queue_capacity_per_worker)),
            opts(o)
        {
            cm = std::make_unique<ConnManager>(std::move(broker), std::move(cid), &err_cb);
            rings.reserve((size_t)workers);
            for (int i = 0; i < workers; ++i) rings.emplace_back(std::make_unique<Ring>(cap));
        }

        // Worker: event-driven, zero sleeps. Honors DisconnectPolicy and policies on overflow.
        void worker_loop(int ring_index) noexcept {
            auto& latch = cm->latch();
            auto& cli = cm->client();
            auto& q = *rings[(size_t)ring_index];

            std::unique_lock<std::mutex> qlk(q.m);

            while (!stopping.load(std::memory_order_acquire)) {
                q.wait_not_empty(qlk, stopping);
                if (stopping.load(std::memory_order_acquire) && q.empty_()) break;

                // If configured to drop while disconnected (QoS0 perf mode), drain one fast.
                if (drop_on_disc(opts.on_disconnect) && !latch.is_connected()) {
                    (void)q.pop_unlocked();
                    qlk.unlock(); q.notify_not_full(); qlk.lock();
                    continue;
                }

                // Reliability mode: wait for connection before popping (prevents retrying the same item).
                if (wait_on_disc(opts.on_disconnect)) {
                    qlk.unlock();
                    latch.wait_connected();
                    qlk.lock();
                    if (stopping.load(std::memory_order_acquire)) break;
                    if (q.empty_()) continue;
                }

                // Pop a task, release ring lock so producers can continue.
                Task t = q.pop_unlocked();
                qlk.unlock(); q.notify_not_full();

                // Publish loop: handle transient disconnects without sleeps.
                for (;;) {
                    if (!latch.is_connected()) {
                        if (drop_on_disc(opts.on_disconnect)) break; // drop
                        latch.wait_connected();
                        if (stopping.load(std::memory_order_acquire)) break;
                    }

                    try {
                        auto msg = mqtt::make_message(t.topic, t.payload);
                        msg->set_qos((int)t.qos);
                        msg->set_retained(t.retained);

                        auto tok = cli.publish(msg);

                        if (t.qos >= QoS::AtLeastOnce) {
                            // Track ACK so Flush() waits even when rings are empty
                            ack.inc();
                            try { tok->wait(); }
                            catch (...) { ack.dec_notify(); throw; }
                            ack.dec_notify();
                        }
                        break; // success
                    }
                    catch (const mqtt::exception& e) {
                        if (auto cb = err_cb.get()) (*cb)(ResultCode::ProtocolError, e.what());
                        // loop continues; latch.wait_connected() above will block until up again.
                        continue;
                    }
                    catch (...) {
                        if (auto cb = err_cb.get()) (*cb)(ResultCode::Unknown, "unknown publish error");
                        continue;
                    }
                }

                qlk.lock();
            }
        }
    };

    PoolQueuePublisher::PoolQueuePublisher(std::string brokerUri,
        std::string clientId,
        PoolQueueOptions opt)
        : impl_(std::make_unique<Impl>(std::move(brokerUri), std::move(clientId), opt)) {
    }

    PoolQueuePublisher::~PoolQueuePublisher() { (void)Disconnect(); }

    Status PoolQueuePublisher::Connect(const std::optional<ConnectionOptions>& opts) noexcept {
        auto st = impl_->cm->Connect(opts);
        if (!st.ok()) return st;

        impl_->stopping.store(false, std::memory_order_release);
        impl_->threads.reserve((size_t)impl_->workers);
        for (int i = 0; i < impl_->workers; ++i)
            impl_->threads.emplace_back([impl = impl_.get(), i]() noexcept { impl->worker_loop(i); });
        return Status{ ResultCode::Ok };
    }

    Status PoolQueuePublisher::Disconnect() noexcept {
        impl_->stopping.store(true, std::memory_order_release);
        for (auto& r : impl_->rings) { r->cv_not_empty.notify_all(); r->cv_not_full.notify_all(); }

        for (auto& t : impl_->threads) if (t.joinable()) t.join();
        impl_->threads.clear();

        (void)impl_->cm->Disconnect();
        return Status{ ResultCode::Ok };
    }

    bool PoolQueuePublisher::IsConnected() const noexcept {
        return impl_->cm->latch().is_connected();
    }

    Status PoolQueuePublisher::Enqueue(std::string_view topic,
        std::string_view payload,
        QoS qos, bool retained) noexcept
    {
        const uint32_t id = impl_->rr.fetch_add(1, std::memory_order_relaxed);
        auto& q = *impl_->rings[(size_t)(id % (uint32_t)impl_->rings.size())];

        Impl::Task t;
        t.topic.assign(topic.data(), topic.size());
        t.payload.assign(payload.data(), payload.size());
        t.qos = qos; t.retained = retained;

        const bool ok = q.push(std::move(t), impl_->opts, impl_->stopping);
        return ok ? Status{ ResultCode::Ok } : Status{ ResultCode::Cancelled };
    }

    Status PoolQueuePublisher::Enqueue(const std::wstring& topic,
        const std::wstring& payload,
        QoS qos, bool retained) noexcept
    {
        const std::string t = Utility::WstringToString(topic);
        const std::string p = Utility::WstringToString(payload);
        if ((t.empty() && !topic.empty()) || (p.empty() && !payload.empty()))
            return Status{ ResultCode::ProtocolError };
        return Enqueue(t, p, qos, retained);
    }

    Status PoolQueuePublisher::Flush(std::chrono::milliseconds timeout) noexcept {
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        // 1) Drain rings
        for (;;) {
            bool all_empty = true;
            for (auto& rq : impl_->rings) {
                std::unique_lock<std::mutex> lk(rq->m);
                if (!rq->empty_()) {
                    all_empty = false;
                    if (std::chrono::steady_clock::now() >= deadline) return Status{ ResultCode::Timeout };
                    rq->wait_empty(lk, impl_->stopping);
                    if (impl_->stopping.load(std::memory_order_acquire)) return Status{ ResultCode::Cancelled };
                }
            }
            if (all_empty) break;
        }

        // 2) Wait for broker ACKs on QoS>=1 items that have been popped but not acked yet
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return Status{ ResultCode::Timeout };
        const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        return impl_->ack.wait_zero(rem);
    }

    void PoolQueuePublisher::SetErrorCallback(ErrorCallback cb) noexcept {
        impl_->err_cb.set(std::move(cb));
    }

} // namespace libmqtt::pub
