#pragma once
/**
 * @file AckTracker.h
 * @brief Tracks QoS>=1 in-flight count to implement precise Flush().
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include "LibMqtt/Core/Types.h"

namespace libmqtt {

    /**
     * @brief Tracks number of QoS>=1 messages awaiting broker ACK.
     *
     * Contract:
     *  - Call inc() after publish() returns a token.
     *  - Call dec_notify() after token->wait() completes or if it throws.
     *  - Flush waits for count() == 0.
     */
    class AckTracker {
    public:
        void inc() { inflight_.fetch_add(1, std::memory_order_acq_rel); }

        void dec_notify() {
            inflight_.fetch_sub(1, std::memory_order_acq_rel);
            std::lock_guard<std::mutex> lk(mu_);
            cv_.notify_all();
        }

        int count() const noexcept { 
            return inflight_.load(std::memory_order_acquire); 
        }

        /// Wait until in-flight count reaches zero or timeout.
        Status wait_zero(std::chrono::milliseconds timeout) {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            std::unique_lock<std::mutex> lk(mu_);
            while (inflight_.load(std::memory_order_acquire) > 0) {
                if (cv_.wait_until(lk, deadline) == std::cv_status::timeout) {
                    return Status{ ResultCode::Timeout };
                }
            }
            return Status{ ResultCode::Ok };
        }

    private:
        std::atomic<int> inflight_{ 0 };
        std::mutex mu_;
        std::condition_variable cv_;
    };

}  // namespace libmqtt
