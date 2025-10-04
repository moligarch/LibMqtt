#pragma once
/**
 * @file ConnectedLatch.h
 * @brief Event-driven connection gate (zero-sleep).
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace libmqtt {

    /**
     * @brief Small event primitive to wait for "connected" state.
     *
     * Usage:
     *  - Producer/consumer calls wait_connected() to block until connected.
     *  - Connection callbacks call set_connected(true/false).
     *  - No sleeps; uses condition_variable to wake precisely on state changes.
     */
    class ConnectedLatch {
    public:
        ConnectedLatch() : up_(false) {}

        /// Set the connection state and notify all waiters.
        void set_connected(bool v) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                up_.store(v, std::memory_order_release);
            }
            cv_.notify_all();
        }

        /// @return true if currently connected (lock-free).
        bool is_connected() const noexcept { 
            return up_.load(std::memory_order_acquire);
        }

        /// Block until connected (returns immediately if already connected).
        void wait_connected() {
            if (is_connected()) return;

            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return up_.load(std::memory_order_acquire); });
        }

        /// Block until deadline; returns false on timeout.
        template<class Clock, class Dur>
        bool wait_connected_until(const std::chrono::time_point<Clock, Dur>& deadline) {
            if (is_connected()) return true;

            std::unique_lock<std::mutex> lk(mu_);
            return cv_.wait_until(lk, deadline, [&] { 
                    return up_.load(std::memory_order_acquire); 
                }
            );
        }

    private:
        std::atomic<bool> up_;
        mutable std::mutex mu_;
        std::condition_variable cv_;
    };

}  // namespace libmqtt