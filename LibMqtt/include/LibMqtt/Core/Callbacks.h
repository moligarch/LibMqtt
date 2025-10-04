#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string_view>
#include "LibMqtt/Core/Types.h"

namespace libmqtt {

    // Hot-path callback signatures
    using MessageCallback = std::function<void(std::string_view topic, std::string_view payload)>;
    using ErrorCallback = std::function<void(ResultCode code, std::string_view what)>;

    // RCU-style callback holder: lock-free read path for hot callbacks
    template <class T>
    class CallbackRCU {
    public:
        void set(T cb) {
            p_.store(std::make_shared<T>(std::move(cb)), std::memory_order_release);
        }
        std::shared_ptr<T> get() const noexcept {
            return p_.load(std::memory_order_acquire);
        }
    private:
        std::atomic<std::shared_ptr<T>> p_{ nullptr };
    };

} // namespace libmqtt
