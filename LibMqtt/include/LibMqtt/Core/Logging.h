#pragma once
#include <atomic>
#include <memory>
#include <string_view>

namespace libmqtt {

    enum class LogLevel { Trace = 0, Debug, Info, Warn, Error, Critical, Off };

    struct Logger {
        virtual ~Logger() = default;
        virtual void log(LogLevel lvl, std::string_view msg) noexcept = 0;
    };

    // Global logger controls (thread-safe)
    void SetLogger(std::shared_ptr<Logger> logger) noexcept;
    std::shared_ptr<Logger> GetLogger() noexcept;
    void SetLogLevel(LogLevel lvl) noexcept;
    LogLevel GetLogLevel() noexcept;

    // Lightweight logging helper
    void Log(LogLevel lvl, std::string_view msg) noexcept;

} // namespace libmqtt

#define LIBMQTT_LOG(lvl, msg) ::libmqtt::Log((lvl), (msg))
