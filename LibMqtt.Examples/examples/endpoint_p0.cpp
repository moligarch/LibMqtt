#include <chrono>
#include <iostream>
#include <thread>
#include "LibMqtt/Facade/Endpoint.h"
#include "examples/common/ConsoleLogger.h"

using namespace std::chrono_literals;

int main() {
    libmqtt::SetLogger(std::make_shared<ConsoleLogger>());
    libmqtt::SetLogLevel(libmqtt::LogLevel::Info);

    const std::string broker = "mqtt://172.16.50.72:1883";
    libmqtt::EndpointOptions opts;
    opts.kind = libmqtt::PublisherKind::PoolZero;

    libmqtt::Endpoint ep(broker, "ex-p0-pub", "ex-p0-sub", opts);

    if (!ep.Connect().ok()) { std::cerr << "connect failed\n"; return 1; }

    std::atomic<int> received{ 0 };
    ep.SetMessageCallback([&](std::string_view, std::string_view) { received.fetch_add(1, std::memory_order_relaxed); });

    ep.Subscribe("examples/p0", libmqtt::QoS::AtLeastOnce);

    // Simulate concurrent callers
    const int threads = 10, iters = 50;
    std::vector<std::thread> th;
    for (int t = 0; t < threads; ++t) {
        th.emplace_back([&, t] {
            for (int i = 0; i < iters; ++i)
                ep.Publish("examples/p0", "payload", libmqtt::QoS::AtLeastOnce);
            });
    }
    for (auto& x : th) x.join();

    ep.Flush(5s);
    std::this_thread::sleep_for(200ms);

    std::cout << "Received: " << received.load() << " / " << threads * iters << "\n";
    ep.Disconnect();
    return 0;
}
