#include <chrono>
#include <iostream>
#include <thread>
#include "LibMqtt/Facade/Endpoint.h"
#include "examples/common/ConsoleLogger.h"

using namespace std::chrono_literals;

int main(int argc, char**) {
    (void)argc;
    libmqtt::SetLogger(std::make_shared<ConsoleLogger>());
    libmqtt::SetLogLevel(libmqtt::LogLevel::Info);

    const std::string broker = "mqtt://127.0.0.1:1883";
    libmqtt::EndpointOptions opts;
    opts.kind = libmqtt::PublisherKind::MultiClient;
    opts.multi.connections = 4; // change as desired

    libmqtt::Endpoint ep(broker, "ex-multi-pub", "ex-multi-sub", opts);

    auto st = ep.Connect();
    if (!st.ok()) { std::cerr << "connect failed\n"; return 1; }

    std::atomic<int> received{ 0 };
    ep.SetMessageCallback([&](std::string_view, std::string_view) { received.fetch_add(1, std::memory_order_relaxed); });

    ep.Subscribe("examples/multi", libmqtt::QoS::AtLeastOnce);

    // publish UTF-8 + wstring overloads
    ep.Publish("examples/multi", "hello-utf8", libmqtt::QoS::AtLeastOnce);
    ep.Publish(L"examples/multi", L"hello-wide", libmqtt::QoS::AtLeastOnce);

    ep.Flush(5s);
    std::this_thread::sleep_for(200ms);

    std::cout << "Received: " << received.load() << "\n";
    ep.Disconnect();
    return 0;
}
