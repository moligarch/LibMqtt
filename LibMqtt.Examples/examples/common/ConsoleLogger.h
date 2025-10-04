#pragma once
#include <iostream>
#include <mutex>
#include <string_view>
#include "LibMqtt/Core/Logging.h"

struct ConsoleLogger final : libmqtt::Logger {
    std::mutex mu;
    void log(libmqtt::LogLevel, std::string_view msg) noexcept override {
        std::lock_guard<std::mutex> lk(mu);
        std::cout << msg << std::endl;
    }
};
