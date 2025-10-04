#pragma once
/**
 * @file Callbacks.h
 * @brief Hot-path callback signatures and lock-free callback holder.
 */

#include <atomic>
#include <functional>
#include <memory>
#include <string_view>
#include "LibMqtt/Core/Types.h"

namespace libmqtt {

    /**
     * @brief Callback to handle incoming MQTT messages.
     *
     * The callback signature for handling incoming messages from the broker.
     * @param topic The topic of the message.
     * @param payload The payload of the message.
     */
    using MessageCallback = std::function<void(std::string_view topic, std::string_view payload)>;

    /**
     * @brief Callback to handle error events.
     *
     * The callback signature for handling error events (e.g., connection failures).
     * @param code The error code representing the failure type.
     * @param what A description of the error.
     */
    using ErrorCallback = std::function<void(ResultCode code, std::string_view what)>;

    /**
     * @brief Lock-free read path for hot callbacks.
     *
     * This class implements a read-copy-update (RCU) style holder for callbacks,
     * allowing multiple threads to read the callback function without locking,
     * while ensuring thread safety for setting the callback.
     *
     * @tparam T The callback type (e.g., MessageCallback, ErrorCallback).
     */
    template <class T>
    class CallbackRCU {
    public:
        /**
         * @brief Set the callback function.
         *
         * This function stores the given callback function in an atomic, lock-free manner.
         * @param cb The callback function to be set.
         */
        void set(T cb) {
            p_.store(std::make_shared<T>(std::move(cb)), std::memory_order_release);
        }

        /**
         * @brief Get the current callback function.
         *
         * This function retrieves the current callback function in a lock-free manner.
         * @return A shared pointer to the callback function.
         */
        std::shared_ptr<T> get() const noexcept {
            return p_.load(std::memory_order_acquire);
        }

    private:
        // Atomic shared pointer to the callback function
        std::atomic<std::shared_ptr<T>> p_{ nullptr };
    };

} // namespace libmqtt