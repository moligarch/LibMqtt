// LibMqtt.Bench.cpp (new-only benchmark)
// Modes: pub_p0, pub_pq, pub_multi, ep_p0, ep_pq, ep_multi
// Receiver is raw Paho listener (isolated).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <filesystem>

#include "mqtt/async_client.h"

// New library surfaces only
#include "LibMqtt/Core/Types.h"
#include "LibMqtt/Pub/PoolZeroPublisher.h"
#include "LibMqtt/Pub/PoolQueuePublisher.h"
#include "LibMqtt/Pub/MultiClientPublisher.h"
#include "LibMqtt/Facade/Endpoint.h"

using namespace std::chrono;

static std::string now_hex16() {
    uint64_t ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(ns));
    return std::string(buf, 16);
}
static uint64_t hex16_to_u64_fast(const char* p) {
    uint64_t x = 0;
    for (int i = 0; i < 16; ++i) {
        char c = p[i];
        unsigned v = (c >= '0' && c <= '9') ? (c - '0') :
            (c >= 'a' && c <= 'f') ? (10 + c - 'a') :
            (c >= 'A' && c <= 'F') ? (10 + c - 'A') : 0;
        x = (x << 4) | v;
    }
    return x;
}
static std::string make_payload(size_t size) {
    std::string s;
    s.reserve(size);
    s += now_hex16();
    if (size > s.size()) s.append(size - s.size(), 'x');
    return s;
}

struct Percentiles { double p50{ 0 }, p95{ 0 }, p99{ 0 }; };
static Percentiles pct_from_ns(std::vector<uint64_t>& ns) {
    Percentiles r{};
    if (ns.empty()) return r;
    auto nth = [&](double q)->double {
        size_t k = static_cast<size_t>(q * (ns.size() - 1));
        std::nth_element(ns.begin(), ns.begin() + k, ns.end());
        return ns[k] / 1e6; // ms
        };
    r.p50 = nth(0.50); r.p95 = nth(0.95); r.p99 = nth(0.99);
    return r;
}

struct Args {
    std::string broker = "mqtt://127.0.0.1:1883";
    std::string topic = "bench/topic";
    int qos = 2;
    int msgs = 2000;
    size_t payload_sz = 200;
    int threads = 8;      // workers for pool_queue; also used for ep_pq
    size_t qcap = 16;     // queue capacity per worker for pool_queue/ep_pq
    int publishers = 4;      // number of connections for multi_client / ep_multi
    std::string user = "";
    std::string pass = "";
    std::string cafile = "";
};
static Args parse_args(int argc, char** argv) {
    Args a;
    auto next = [&](int& i)->const char* { if (i + 1 < argc) return argv[++i]; std::exit(2); };
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        if (k == "--broker") a.broker = next(i);
        else if (k == "--topic") a.topic = next(i);
        else if (k == "--qos") a.qos = std::atoi(next(i));
        else if (k == "--msg") a.msgs = std::atoi(next(i));
        else if (k == "--size") a.payload_sz = static_cast<size_t>(std::atoll(next(i)));
        else if (k == "--threads") a.threads = std::atoi(next(i));
        else if (k == "--qcap") a.qcap = static_cast<size_t>(std::atoll(next(i)));
        else if (k == "--pubs") a.publishers = std::atoi(next(i));
        else if (k == "--user") a.user = next(i);
        else if (k == "--pass") a.pass = next(i);
        else if (k == "--cafile") a.cafile = next(i);
        else { std::cerr << "Unknown arg: " << k << "\n"; std::exit(2); }
    }
    return a;
}

// -------------------- Raw Paho Listener (isolated) --------------------------
struct RawPahoListener : public virtual mqtt::callback {
    mqtt::async_client client;
    std::atomic<int> recv_count{ 0 };
    std::vector<uint64_t> e2e_ns;
    std::atomic<size_t> e2e_idx{ 0 };
    time_point<steady_clock> last_recv;

    RawPahoListener(const Args& args, const std::string& cid, int expected_total)
        : client(args.broker, cid)
    {
        client.set_callback(*this);

        auto b = mqtt::connect_options_builder().clean_session().automatic_reconnect(false);
        if (args.broker.rfind("mqtts://", 0) == 0) {
            if (!args.user.empty()) { b.user_name(args.user); b.password(args.pass); }
            mqtt::ssl_options sslopts;
            if (!args.cafile.empty()) sslopts.set_trust_store(args.cafile);
            sslopts.set_enable_server_cert_auth(true);
            b.ssl(sslopts);
        }
        client.connect(b.finalize())->wait();

        const size_t cap = std::min(static_cast<size_t>(expected_total), static_cast<size_t>(200000));
        e2e_ns.resize(cap);
    }

    void subscribe(const Args& args, const std::string& topic, int qos) {
        client.subscribe(topic, qos)->wait();
        std::this_thread::sleep_for(300ms);
    }

    ~RawPahoListener() override {
        try { client.disconnect()->wait(); }
        catch (...) {}
    }

    void message_arrived(mqtt::const_message_ptr msg) override {
        const std::string& payload = msg->get_payload_str();
        if (payload.size() >= 16) {
            uint64_t t0 = hex16_to_u64_fast(payload.data());
            uint64_t t1 = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
            size_t i = e2e_idx.fetch_add(1, std::memory_order_relaxed);
            if (i < e2e_ns.size()) e2e_ns[i] = (t1 - t0);
        }
        last_recv = steady_clock::now();
        recv_count.fetch_add(1, std::memory_order_relaxed);
    }
};

// -------------------- Result + printing -------------------------------------
struct RunResult {
    std::string mode;
    int total{ 0 };
    int received{ 0 };
    double send_elapsed_s{ 0 };   // from first send to send completion
    double recv_elapsed_s{ 0 };   // from first send to last receive (goodput timeline)
    double throughput_send{ 0 };
    double throughput_recv{ 0 };
    Percentiles local;   // only for modes that measure local publish latency
    Percentiles e2e;
    bool missed{ false };
};

static void print_summary(const std::vector<RunResult>& R) {
    const int W_MODE = 14;
    const int W_NUM = 12;
    const int W_RAT = 12;

    auto hdr = [&]() {
        std::cout << "\n==================== SUMMARY ====================\n";
        std::cout << std::left << std::setw(W_MODE) << "Mode"
            << std::right << std::setw(W_NUM) << "Thr_send"
            << std::right << std::setw(W_NUM) << "Thr_recv"
            << std::right << std::setw(W_RAT) << "Recv/Total"
            << std::right << std::setw(W_NUM) << "p50_e2e"
            << std::right << std::setw(W_NUM) << "p95_e2e"
            << std::right << std::setw(W_NUM) << "p99_e2e"
            << std::right << std::setw(W_NUM) << "p50_local"
            << std::right << std::setw(W_NUM) << "p95_local"
            << std::right << std::setw(W_NUM) << "p99_local"
            << "\n";
        };

    auto row = [&](const RunResult& r) {
        std::ostringstream ratio; ratio << r.received << "/" << r.total;
        std::cout << std::left << std::setw(W_MODE) << r.mode
            << std::right << std::setw(W_NUM) << std::fixed << std::setprecision(1) << r.throughput_send
            << std::right << std::setw(W_NUM) << std::fixed << std::setprecision(1) << r.throughput_recv
            << std::right << std::setw(W_RAT) << ratio.str()
            << std::right << std::setw(W_NUM) << std::fixed << std::setprecision(2) << r.e2e.p50
            << std::right << std::setw(W_NUM) << r.e2e.p95
            << std::right << std::setw(W_NUM) << r.e2e.p99
            << std::right << std::setw(W_NUM) << r.local.p50
            << std::right << std::setw(W_NUM) << r.local.p95
            << std::right << std::setw(W_NUM) << r.local.p99
            << "\n";
        };

    hdr();
    for (const auto& r : R) row(r);
    std::cout << "=================================================\n";
}

// -------------- Helpers common to all runs ----------------------------------
static void finalize_result(RunResult& r, RawPahoListener& L, time_point<steady_clock> first_send) {
    const auto deadline = steady_clock::now() + 10s;
    while (L.recv_count.load() < r.total && steady_clock::now() < deadline) {
        std::this_thread::sleep_for(50ms);
    }
    r.received = L.recv_count.load();

    // recv_elapsed is from first send to last receive
    r.recv_elapsed_s = (r.received > 0)
        ? duration_cast<duration<double>>(L.last_recv - first_send).count()
        : 0.0;

    L.e2e_ns.resize(std::min(L.e2e_ns.size(), static_cast<size_t>(r.received)));
    r.e2e = pct_from_ns(L.e2e_ns);

    r.throughput_send = (r.received > 0 && r.send_elapsed_s > 0) ? (r.received / r.send_elapsed_s) : 0.0;
    r.throughput_recv = (r.received > 0 && r.recv_elapsed_s > 0) ? (r.received / r.recv_elapsed_s) : 0.0;
    r.missed = (r.received != r.total);
}

static std::optional<libmqtt::ConnectionOptions> build_opts(const Args& a) {
    if (a.broker.rfind("mqtts://", 0) == 0) {
        libmqtt::ConnectionOptions o; if (!a.user.empty()) { o.username = a.user; o.password = a.pass; }
        libmqtt::TlsOptions tls;
        tls.ca_file_path = a.cafile; // std::filesystem::path compatible
        tls.enable_server_cert_auth = true;
        o.tls = tls;
        return o;
    }
    return std::nullopt;
}

// -------------------- Publisher (direct) modes ------------------------------
static RunResult run_pub_p0(const Args& args) {
    RunResult r; r.mode = "pub_p0"; r.total = args.msgs;

    RawPahoListener L(args, "bench-p0-listener", r.total);
    L.subscribe(args, args.topic, args.qos);

    libmqtt::pub::PoolZeroPublisher pub(args.broker, "bench-p0");
    auto pst = pub.Connect(build_opts(args));
    if (!pst.ok()) throw std::runtime_error("pub_p0: connect failed");

    std::vector<uint64_t> local_ns; local_ns.reserve(r.total);

    const auto first_send = steady_clock::now();
    const auto t0 = first_send;
    for (int i = 0; i < r.total; ++i) {
        auto payload = make_payload(args.payload_sz);
        auto s0 = steady_clock::now();
        auto st = pub.Publish(args.topic, payload, static_cast<libmqtt::QoS>(args.qos), false);
        if (args.qos >= 1) local_ns.push_back(duration_cast<nanoseconds>(steady_clock::now() - s0).count());
        if (!st.ok()) throw std::runtime_error("pub_p0: publish failed");
    }
    const auto t1 = steady_clock::now();
    pub.Disconnect();

    r.send_elapsed_s = duration_cast<duration<double>>(t1 - t0).count();
    r.local = pct_from_ns(local_ns);
    finalize_result(r, L, first_send);
    return r;
}

static RunResult run_pub_pq(const Args& args) {
    RunResult r; r.mode = "pub_pq"; r.total = args.msgs;

    RawPahoListener L(args, "bench-pq-listener", r.total);
    L.subscribe(args, args.topic, args.qos);

    libmqtt::pub::PoolQueueOptions qo; qo.workers = std::max(1, args.threads); qo.queue_capacity_per_worker = std::max<size_t>(1, args.qcap);
    libmqtt::pub::PoolQueuePublisher pub(args.broker, "bench-pq", qo);
    auto pst = pub.Connect(build_opts(args));
    if (!pst.ok()) throw std::runtime_error("pub_pq: connect failed");

    const auto first_send = steady_clock::now();
    const auto t0 = first_send;
    for (int i = 0; i < r.total; ++i) {
        auto st = pub.Enqueue(args.topic, make_payload(args.payload_sz), static_cast<libmqtt::QoS>(args.qos), false);
        if (!st.ok()) throw std::runtime_error("pub_pq: enqueue failed");
    }
    pub.Flush(10s);
    const auto t1 = steady_clock::now();
    pub.Disconnect();

    r.send_elapsed_s = duration_cast<duration<double>>(t1 - t0).count();
    finalize_result(r, L, first_send);
    return r;
}

static RunResult run_pub_multi(const Args& args) {
    RunResult r; r.mode = "pub_multi"; r.total = args.msgs;

    RawPahoListener L(args, "bench-multi-listener", r.total);
    L.subscribe(args, args.topic, args.qos);

    libmqtt::pub::MultiClientOptions mc; mc.connections = std::max(1, args.publishers);
    libmqtt::pub::MultiClientPublisher pub(args.broker, "bench-multi", mc);
    auto pst = pub.Connect(build_opts(args));
    if (!pst.ok()) throw std::runtime_error("pub_multi: connect failed");

    // Use N threads to drive the K connections
    const int threads = std::max(1, args.publishers);
    std::vector<std::thread> th; th.reserve(threads);
    std::vector<uint64_t> local_all; local_all.reserve(r.total);

    const auto first_send = steady_clock::now();
    const auto t0 = first_send;

    for (int t = 0; t < threads; ++t) {
        th.emplace_back([&, t]() {
            std::vector<uint64_t> local_ns; local_ns.reserve(r.total / threads + 8);
            for (int i = t; i < r.total; i += threads) {
                auto payload = make_payload(args.payload_sz);
                auto s0 = steady_clock::now();
                auto st = pub.Publish(args.topic, payload, static_cast<libmqtt::QoS>(args.qos), false);
                if (args.qos >= 1) local_ns.push_back(duration_cast<nanoseconds>(steady_clock::now() - s0).count());
                if (!st.ok()) {
                    if (args.qos == 0) { /* ++qos0_failures; continue; */ }
                    else throw std::runtime_error("pub_multi: publish failed");
                }
            }
            static std::mutex m; std::lock_guard<std::mutex> lk(m);
            local_all.insert(local_all.end(), local_ns.begin(), local_ns.end());
            });
    }
    for (auto& x : th) x.join();

    pub.Flush(10s);
    const auto t1 = steady_clock::now();
    pub.Disconnect();

    r.send_elapsed_s = duration_cast<duration<double>>(t1 - t0).count();
    r.local = pct_from_ns(local_all);
    finalize_result(r, L, first_send);
    return r;
}

// -------------------- Endpoint modes ----------------------------------------
static std::optional<libmqtt::ConnectionOptions> build_opts_ep(const Args& a) {
    return (a.broker.rfind("mqtts://", 0) == 0) ? build_opts(a) : std::nullopt;
}
static RunResult run_endpoint(const Args& args, libmqtt::PublisherKind kind, const char* label) {
    RunResult r; r.mode = label; r.total = args.msgs;

    RawPahoListener L(args, std::string("bench-") + label + "-listener", r.total);
    L.subscribe(args, args.topic, args.qos);

    libmqtt::EndpointOptions eopts;
    eopts.kind = kind;
    if (kind == libmqtt::PublisherKind::PoolQueue) {
        eopts.poolQueue.workers = std::max(1, args.threads);
        eopts.poolQueue.queue_capacity_per_worker = std::max<size_t>(1, args.qcap);
    }
    else if (kind == libmqtt::PublisherKind::MultiClient) {
        eopts.multi.connections = std::max(1, args.publishers);
    }
    eopts.conn = build_opts(args);

    libmqtt::Endpoint ep(args.broker, std::string("bench-") + label + "-pub", std::string("bench-") + label + "-sub", eopts);
    if (!ep.Connect().ok()) throw std::runtime_error("endpoint connect failed");

    // Concurrency model: match pub_multi behavior for MultiClient; for PoolQueue,
    // drive with args.threads; for PoolZero keep 1 thread (single connection).
    int conc = 1;
    switch (kind) {
    case libmqtt::PublisherKind::MultiClient: conc = std::max(1, args.publishers); break;
    case libmqtt::PublisherKind::PoolQueue:  conc = std::max(1, args.threads);     break;
    case libmqtt::PublisherKind::PoolZero:   conc = 1;                              break;
    }

    std::vector<std::thread> th; th.reserve(conc);
    std::atomic<bool> first_set{ false };
    std::atomic<int>  launched{ 0 };
    time_point<steady_clock> first_send;

    auto worker = [&](int tid) {
        // simple striding to split r.total across conc threads
        for (int i = tid; i < r.total; i += conc) {
            auto payload = make_payload(args.payload_sz);
            // capture time of the very first publish across all threads
            if (!first_set.load(std::memory_order_acquire)) {
                auto t = steady_clock::now();
                bool exp = false;
                if (first_set.compare_exchange_strong(exp, true, std::memory_order_acq_rel)) {
                    first_send = t;
                }
            }
            ep.Publish(args.topic, payload, static_cast<libmqtt::QoS>(args.qos), false);
        }
        };

    // start workers and measure send window
    const auto t0 = steady_clock::now();
    for (int t = 0; t < conc; ++t) th.emplace_back(worker, t);
    for (auto& x : th) x.join();
    ep.Flush(10s);
    const auto t1 = steady_clock::now();

    ep.Disconnect();

    // if no thread observed first_send (very unlikely), fallback to t0
    if (!first_set.load(std::memory_order_acquire)) first_send = t0;

    r.send_elapsed_s = duration_cast<duration<double>>(t1 - t0).count();
    finalize_result(r, L, first_send);
    return r;
}

// ------------------------------ Main ----------------------------------------
int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);

    std::cout << "Broker   : " << args.broker << "\n"
        << "Topic    : " << args.topic << "\n"
        << "QoS      : " << args.qos << "\n"
        << "Msgs     : " << args.msgs << "\n"
        << "Payload  : " << args.payload_sz << " bytes\n"
        << "Threads  : " << args.threads << " (pool)\n"
        << "Connections: " << args.publishers << " (multi)\n";
    if (!args.cafile.empty()) std::cout << "TLS CA   : " << args.cafile << "\n";

    std::vector<RunResult> results;
    try {
        // Direct publisher modes
        results.push_back(run_pub_p0(args));
        std::this_thread::sleep_for(200ms);

        results.push_back(run_pub_pq(args));
        std::this_thread::sleep_for(200ms);

        results.push_back(run_pub_multi(args));
        std::this_thread::sleep_for(200ms);

        // Endpoint variants
        results.push_back(run_endpoint(args, libmqtt::PublisherKind::PoolZero, "ep_p0"));
        std::this_thread::sleep_for(200ms);
        results.push_back(run_endpoint(args, libmqtt::PublisherKind::PoolQueue, "ep_pq"));
        std::this_thread::sleep_for(200ms);
        results.push_back(run_endpoint(args, libmqtt::PublisherKind::MultiClient, "ep_multi"));
    }
    catch (const std::exception& ex) {
        std::cerr << "Benchmark error: " << ex.what() << "\n";
        return 10;
    }

    print_summary(results);
    for (const auto& r : results) if (r.missed) return 4;
    return 0;
}
