#include "LibMqtt/Core/Logging.h"

namespace libmqtt {
    namespace {
        std::atomic<std::shared_ptr<Logger>> g_logger{ nullptr };
        std::atomic<int> g_level{ static_cast<int>(LogLevel::Warn) }; // default: Warn+
    }

    void SetLogger(std::shared_ptr<Logger> logger) noexcept {
        g_logger.store(std::move(logger), std::memory_order_release);
    }
    std::shared_ptr<Logger> GetLogger() noexcept {
        return g_logger.load(std::memory_order_acquire);
    }
    void SetLogLevel(LogLevel lvl) noexcept {
        g_level.store(static_cast<int>(lvl), std::memory_order_release);
    }
    LogLevel GetLogLevel() noexcept {
        return static_cast<LogLevel>(g_level.load(std::memory_order_acquire));
    }

    void Log(LogLevel lvl, std::string_view msg) noexcept {
        if (static_cast<int>(lvl) < g_level.load(std::memory_order_relaxed)) return;
        if (auto lg = g_logger.load(std::memory_order_acquire)) {
            lg->log(lvl, msg);
        }
    }

} // namespace libmqtt
