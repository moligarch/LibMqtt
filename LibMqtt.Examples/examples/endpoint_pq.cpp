#include <chrono>
#include <iostream>
#include <thread>
#include "LibMqtt/Facade/Endpoint.h"
#include "examples/common/ConsoleLogger.h"

using namespace std::chrono_literals;

int main() {
    libmqtt::SetLogger(std::make_shared<ConsoleLogger>());
    libmqtt::SetLogLevel(libmqtt::LogLevel::Info);

    const std::string broker = "mqtt://127.0.0.1:1883";
    libmqtt::EndpointOptions opts;
    opts.kind = libmqtt::PublisherKind::PoolQueue;
    opts.poolQueue.workers = 4;
    opts.poolQueue.queue_capacity_per_worker = 16;

    libmqtt::Endpoint ep(broker, "ex-pq-pub", "ex-pq-sub", opts);

    if (!ep.Connect().ok()) { std::cerr << "connect failed\n"; return 1; }

    std::atomic<int> received{ 0 };
    ep.SetMessageCallback([&](std::string_view, std::string_view) { received.fetch_add(1, std::memory_order_relaxed); });

    ep.Subscribe("examples/pq", libmqtt::QoS::AtLeastOnce);

    const int total = 500;
    for (int i = 0; i < total; ++i)
        ep.Publish("examples/pq", "payload", libmqtt::QoS::AtLeastOnce); // Enqueue() path

    ep.Flush(5s);
    std::this_thread::sleep_for(300ms);

    std::cout << "Received: " << received.load() << " / " << total << "\n";
    ep.Disconnect();
    return 0;
}
