#include "pch.h"
#include "CppUnitTest.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// LibMqtt umbrella
#include "LibMqtt/LibMqtt.h"

// Paho for the test-side subscriber helper
#include "mqtt/async_client.h"

#include "utils.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace UnitTest
{
    // -----------------------------------------------------------------------------
    // Broker endpoints from our docker-compose setup
    // -----------------------------------------------------------------------------
    static const char* UNSECURE_NO_AUTH_BROKER   = "mqtt://localhost:1883";
    static const char* UNSECURE_WITH_AUTH_BROKER = "mqtt://localhost:1882";
    static const char* SECURE_WITH_AUTH_BROKER   = "mqtts://localhost:8883";
    static const char* SECURE_NO_AUTH_BROKER     = "mqtts://localhost:8884";

    // -----------------------------------------------------------------------------
    // Small test-side subscriber helper (Paho-based) used only by tests
    // -----------------------------------------------------------------------------
    class TestSubscriber : public virtual mqtt::callback {
    public:
        explicit TestSubscriber(std::string broker, std::string clientId)
            : cli_(std::move(broker), std::move(clientId)) {
            cli_.set_callback(*this);
        }

        // mqtt::callback
        void connected(const std::string&) override {
            connected_.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(mu_);
                // no-op
            }
            cv_.notify_all();
        }
        void connection_lost(const std::string&) override {
            connected_.store(false, std::memory_order_release);
            cv_.notify_all();
        }
        void message_arrived(mqtt::const_message_ptr msg) override {
            if (cb_) cb_(msg->get_topic(), msg->to_string());
            recv_count_.fetch_add(1, std::memory_order_acq_rel);
        }
        void delivery_complete(mqtt::delivery_token_ptr) override {}

        void SetMessageCallback(libmqtt::MessageCallback cb) {
            cb_ = std::move(cb);
        }

        libmqtt::Status Connect(const std::optional<libmqtt::ConnectionOptions>& opts = std::nullopt) {
            try {
                auto copts = libmqtt::make_connect_options(opts);
                cli_.connect(copts)->wait();
                connected_.store(true, std::memory_order_release);
                cv_.notify_all();
                return { libmqtt::ResultCode::Ok };
            }
            catch (...) {
                return { libmqtt::ResultCode::Disconnected };
            }
        }

        void Disconnect() {
            try { if (cli_.is_connected()) cli_.disconnect()->wait(); }
            catch (...) {}
            connected_.store(false, std::memory_order_release);
            cv_.notify_all();
        }

        bool IsConnected() const noexcept { return connected_.load(std::memory_order_acquire); }

        void WaitConnected(int timeout_sec = 5) {
            if (IsConnected()) return;
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::seconds(timeout_sec), [&] { return IsConnected(); });
        }

        void Subscribe(const std::string& topic, int qos = 1) {
            cli_.subscribe(topic, qos)->wait();
        }

        int ReceivedCount() const noexcept { return recv_count_.load(std::memory_order_acquire); }

    private:
        mqtt::async_client           cli_;
        std::atomic<bool>            connected_{ false };
        std::atomic<int>             recv_count_{ 0 };
        libmqtt::MessageCallback     cb_;
        std::condition_variable      cv_;
        mutable std::mutex           mu_;
    };

    // -----------------------------------------------------------------------------
    // Common test utilities
    // -----------------------------------------------------------------------------
    class TestBase {
    public:
        // Publisher-side wait (polling is fine for tests)
        template <class P>
        bool WaitForConnection(P& pub, int timeout_sec = 5) {
            for (int i = 0; i < timeout_sec * 10; ++i) {
                if (pub.IsConnected()) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            return false;
        }

        std::filesystem::path GetCertPath(const std::string& certName) {
            // Adjust relative path to your repo cert location if needed.
            auto p = std::filesystem::path("../../../../mosquitto/certs/") / certName;
            if (!std::filesystem::exists(p)) {
                Logger::WriteMessage(("Certificate not found at: " + p.string()).c_str());
                return {};
            }
            return p;
        }

        libmqtt::ConnectionOptions MakeAuthTls(bool withAuth) {
            libmqtt::ConnectionOptions opts;
            if (withAuth) { opts.username = "testuser"; opts.password = "testpass"; }
            libmqtt::TlsOptions tls;
            tls.ca_file_path = GetCertPath("ca.crt");
            Assert::IsFalse(tls.ca_file_path.empty(), L"CA certificate not found for test.");
            opts.tls = tls;
            return opts;
        }
    };

    // ============================================================================
    // CONNECTIVITY
    // ============================================================================
    TEST_CLASS(ConnectivityTests), public TestBase{
    public:
      TEST_METHOD(Unsecure_NoAuth_ConnectPublishSubscribe) {
          // Publisher: PoolZero (single connection, no queue)
          libmqtt::pub::PoolZeroPublisher pub(UNSECURE_NO_AUTH_BROKER, "conn-unsecure-noauth");
          Assert::IsTrue(pub.Connect().ok(), L"Publisher failed to connect.");
          Assert::IsTrue(WaitForConnection(pub), L"Publisher failed to report connected.");

          // Subscriber
          TestSubscriber sub(UNSECURE_NO_AUTH_BROKER, "sub-unsecure-noauth");
          sub.SetMessageCallback([](std::string_view, std::string_view) {});
          Assert::IsTrue(sub.Connect().ok(), L"Subscriber failed to connect.");
          sub.WaitConnected();

          const std::string TOPIC = "connectivity/test";
          sub.Subscribe(TOPIC, /*qos*/1);

          // Publish and verify
          const std::string msg = "hello-zero";
          Assert::IsTrue(pub.Publish(TOPIC, msg, libmqtt::QoS::AtLeastOnce).ok(), L"Publish failed.");

          // Wait for message
          for (int i = 0; i < 20 && sub.ReceivedCount() < 1; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

          Assert::AreEqual(1, sub.ReceivedCount(), L"Did not receive published message.");
          (void)pub.Disconnect(); sub.Disconnect();
        }

        TEST_METHOD(Unsecure_WithAuth_ConnectPublishSubscribe) {
          auto opts = libmqtt::ConnectionOptions{ "testuser", "testpass", std::nullopt };
          libmqtt::pub::PoolZeroPublisher pub(UNSECURE_WITH_AUTH_BROKER, "conn-unsecure-auth");
          Assert::IsTrue(pub.Connect(opts).ok(), L"Publisher failed to connect.");
          Assert::IsTrue(WaitForConnection(pub), L"Publisher failed to report connected.");

          TestSubscriber sub(UNSECURE_WITH_AUTH_BROKER, "sub-unsecure-auth");
          sub.SetMessageCallback([](std::string_view, std::string_view) {});
          Assert::IsTrue(sub.Connect(opts).ok(), L"Subscriber failed to connect.");
          sub.WaitConnected();

          const std::string TOPIC = "connectivity/test-auth";
          sub.Subscribe(TOPIC, 1);

          Assert::IsTrue(pub.Publish(TOPIC, "hello-auth", libmqtt::QoS::AtLeastOnce).ok(), L"Publish failed.");
          for (int i = 0; i < 20 && sub.ReceivedCount() < 1; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          Assert::AreEqual(1, sub.ReceivedCount(), L"Did not receive message.");
          (void)pub.Disconnect(); sub.Disconnect();
        }

        TEST_METHOD(Secure_NoAuth_ConnectPublishSubscribe) {
          auto opts = MakeAuthTls(/*withAuth*/false);
          libmqtt::pub::PoolZeroPublisher pub(SECURE_NO_AUTH_BROKER, "conn-secure-noauth");
          Assert::IsTrue(pub.Connect(opts).ok(), L"Publisher failed to connect.");
          Assert::IsTrue(WaitForConnection(pub), L"Publisher failed to report connected.");

          TestSubscriber sub(SECURE_NO_AUTH_BROKER, "sub-secure-noauth");
          sub.SetMessageCallback([](std::string_view, std::string_view) {});
          Assert::IsTrue(sub.Connect(opts).ok(), L"Subscriber failed to connect.");
          sub.WaitConnected();

          const std::string TOPIC = "connectivity/test-secure";
          sub.Subscribe(TOPIC, 1);

          Assert::IsTrue(pub.Publish(TOPIC, "hello-secure", libmqtt::QoS::AtLeastOnce).ok(), L"Publish failed.");
          for (int i = 0; i < 20 && sub.ReceivedCount() < 1; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          Assert::AreEqual(1, sub.ReceivedCount(), L"Did not receive message.");
          (void)pub.Disconnect(); sub.Disconnect();
        }

        TEST_METHOD(Secure_WithAuth_ConnectPublishSubscribe) {
          auto opts = MakeAuthTls(/*withAuth*/true);
          libmqtt::pub::PoolZeroPublisher pub(SECURE_WITH_AUTH_BROKER, "conn-secure-auth");
          Assert::IsTrue(pub.Connect(opts).ok(), L"Publisher failed to connect.");
          Assert::IsTrue(WaitForConnection(pub), L"Publisher failed to report connected.");

          TestSubscriber sub(SECURE_WITH_AUTH_BROKER, "sub-secure-auth");
          sub.SetMessageCallback([](std::string_view, std::string_view) {});
          Assert::IsTrue(sub.Connect(opts).ok(), L"Subscriber failed to connect.");
          sub.WaitConnected();

          const std::string TOPIC = "connectivity/test-secure-auth";
          sub.Subscribe(TOPIC, 1);

          Assert::IsTrue(pub.Publish(TOPIC, "hello-secure-auth", libmqtt::QoS::AtLeastOnce).ok(), L"Publish failed.");
          for (int i = 0; i < 20 && sub.ReceivedCount() < 1; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          Assert::AreEqual(1, sub.ReceivedCount(), L"Did not receive message.");
          (void)pub.Disconnect(); sub.Disconnect();
        }

        TEST_METHOD(WString_Overload_ConnectPublishSubscribe) {
            // PoolZeroPublisher exposes wstring overloads at the edge
            libmqtt::pub::PoolZeroPublisher pub("mqtt://localhost:1883", "conn-wstring-overload");
            Assert::IsTrue(pub.Connect().ok(), L"Publisher failed to connect.");
            Assert::IsTrue(WaitForConnection(pub), L"Publisher failed to report connected.");

            TestSubscriber sub(UNSECURE_NO_AUTH_BROKER, "sub-wstring");
            std::atomic<bool> got{false};
            std::string payload_utf8;
            std::mutex m;
            sub.SetMessageCallback([&](std::string_view, std::string_view payload) {
              std::lock_guard<std::mutex> lk(m);
              payload_utf8.assign(payload.data(), payload.size());
              got.store(true, std::memory_order_release);
            });
            Assert::IsTrue(sub.Connect().ok(), L"Subscriber failed to connect.");
            sub.WaitConnected();

            const std::wstring TOPIC = L"connectivity/wstring";
            const std::wstring MESSAGE = L"hello-wstring";
            sub.Subscribe(Utility::WstringToString(TOPIC), 1);

            Assert::IsTrue(pub.Publish(TOPIC, MESSAGE, libmqtt::QoS::AtLeastOnce).ok(), L"Publish failed.");

            for (int i = 0; i < 20 && !got.load(std::memory_order_acquire); ++i)
              std::this_thread::sleep_for(std::chrono::milliseconds(100));

            Assert::IsTrue(got.load(), L"No message received.");
            Assert::AreEqual(Utility::WstringToString(MESSAGE), payload_utf8, L"Payload mismatch.");
            (void)pub.Disconnect(); sub.Disconnect();
          }
    };

    // ============================================================================
    // ERROR HANDLING
    // ============================================================================
    TEST_CLASS(ErrorHandlingTests), public TestBase{
    public:
      TEST_METHOD(ConnectionFailure_BadPort) {
        libmqtt::pub::PoolZeroPublisher pub("mqtt://localhost:9999", "error-bad-port");

        std::atomic<bool> err_called{false};
        pub.SetErrorCallback([&](libmqtt::ResultCode, std::string_view) { err_called = true; });

        auto st = pub.Connect();
        Assert::IsFalse(st.ok(), L"Connect should fail on bad port.");
        Assert::IsFalse(pub.IsConnected(), L"Should not be connected.");
        Assert::IsTrue(err_called.load(), L"Error callback was not invoked on connection failure.");
      }

      TEST_METHOD(ConnectionFailure_BadCredentials) {
        libmqtt::pub::PoolZeroPublisher pub(UNSECURE_WITH_AUTH_BROKER, "error-bad-creds");

        libmqtt::ConnectionOptions opts;
        opts.username = "testuser";
        opts.password = "wrongpassword";

        std::atomic<bool> err_called{false};
        pub.SetErrorCallback([&](libmqtt::ResultCode, std::string_view) { err_called = true; });

        auto st = pub.Connect(opts);
        Assert::IsFalse(st.ok(), L"Connect should fail with bad credentials.");
        Assert::IsFalse(pub.IsConnected(), L"Should not be connected with bad credentials.");
        Assert::IsTrue(err_called.load(), L"Error callback was not invoked on auth failure.");
      }

      TEST_METHOD(Publish_WithoutConnection_FailsGracefully) {
          // For PoolZeroPublisher, Publish() will block waiting for connection,
          // so use MultiClientPublisher which returns Disconnected when not connected.
          libmqtt::pub::MultiClientPublisher multi(UNSECURE_NO_AUTH_BROKER, "no-connect", {.connections = 2 });

          auto st = multi.Publish("some/topic", "some/payload", libmqtt::QoS::AtLeastOnce);
          Assert::IsFalse(st.ok(), L"Publish should fail gracefully when not connected.");
          Assert::AreEqual((int)libmqtt::ResultCode::Disconnected, (int)st.code, L"Expected Disconnected result.");
        }
    };

    // ============================================================================
    // RACE CONDITIONS & THREAD SAFETY
    // ============================================================================
    TEST_CLASS(RaceConditionTests), public TestBase{
    public:
      TEST_METHOD(ConcurrentSetErrorCallback) {
        libmqtt::pub::MultiClientPublisher multi(UNSECURE_NO_AUTH_BROKER, "race-set-cb", {.connections = 3 });
        Assert::IsTrue(multi.Connect().ok(), L"Connect failed.");
        Assert::IsTrue(WaitForConnection(multi), L"Not connected.");

        std::atomic<bool> stop{false};

        std::thread t1([&] {
          for (int i = 0; i < 1000; ++i) {
            (void)multi.Publish("race/callback", "msg", libmqtt::QoS::AtMostOnce);
          }
          stop = true;
        });

        std::thread t2([&] {
          while (!stop) {
            multi.SetErrorCallback([](libmqtt::ResultCode, std::string_view) {});
          }
        });

        t1.join();
        stop = true;
        t2.join();

        (void)multi.Disconnect();
        Assert::IsTrue(true); // Completed without crash/deadlock.
      }

      TEST_METHOD(ConnectionStateResilienceTest) {
          // PoolZeroPublisher: publishing thread runs continuously; connection toggles.
          libmqtt::pub::PoolZeroPublisher pub(UNSECURE_NO_AUTH_BROKER, "resilience-zero");
          (void)pub.Connect();
          Assert::IsTrue(WaitForConnection(pub), L"Not connected.");

          std::atomic<bool> stop{false};

          std::thread mgr([&] {
            for (int i = 0; i < 6; ++i) {
              (void)pub.Disconnect();
              std::this_thread::sleep_for(std::chrono::milliseconds(150));
              (void)pub.Connect();
              std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
            stop = true;
          });

          std::thread prod([&] {
            while (!stop) {
                // QoS0 so we don't block on ACK; PoolZero waits for connection internally.
                (void)pub.Publish("resilience/topic", "data", libmqtt::QoS::AtMostOnce);
              }
            });

            mgr.join();
            stop = true;
            prod.join();

            (void)pub.Disconnect();
            Assert::IsTrue(true); // Completed without deadlock.
          }
    };

    // ============================================================================
    // STRESS TESTS
    // ============================================================================
    TEST_CLASS(StressTests), public TestBase{
    public:
      TEST_METHOD(HighThroughput_SinglePublisher) {
          // Listener
          TestSubscriber sub(UNSECURE_NO_AUTH_BROKER, "stress-single-sub");
          std::atomic<int> recv{0};
          sub.SetMessageCallback([&](std::string_view, std::string_view) { recv.fetch_add(1, std::memory_order_acq_rel); });
          Assert::IsTrue(sub.Connect().ok(), L"Sub connect failed."); sub.WaitConnected();

          const std::string TOPIC = "stress/single";
          sub.Subscribe(TOPIC, 1);

          // Publisher (PoolZero)
          libmqtt::pub::PoolZeroPublisher pub(UNSECURE_NO_AUTH_BROKER, "stress-single-pub");
          Assert::IsTrue(pub.Connect().ok(), L"Pub connect failed."); Assert::IsTrue(WaitForConnection(pub), L"Not connected.");

          const int ITERS = 1000;
          std::thread t([&] {
            for (int i = 0; i < ITERS; ++i)
              (void)pub.Publish(TOPIC, "message", libmqtt::QoS::ExactlyOnce);
          });
          t.join();

          // Wait for all messages
          for (int i = 0; i < 100 && recv.load() != ITERS; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

          Assert::AreEqual(ITERS, recv.load(), L"Not all messages received.");
          (void)pub.Disconnect(); sub.Disconnect();
        }

        TEST_METHOD(HighThroughput_MultipleConnections) {
            // Listener
            TestSubscriber sub(UNSECURE_NO_AUTH_BROKER, "stress-multi-sub");
            std::atomic<int> recv{0};
            sub.SetMessageCallback([&](std::string_view, std::string_view) { recv.fetch_add(1, std::memory_order_acq_rel); });
            Assert::IsTrue(sub.Connect().ok(), L"Sub connect failed."); sub.WaitConnected();

            const std::string TOPIC = "stress/multi";
            sub.Subscribe(TOPIC, 1);

            // MultiClientPublisher across N connections
            const int NUM_CONNS = 5;
            const int MSG_PER_CONN = 200;
            const int TOTAL = NUM_CONNS * MSG_PER_CONN;

            libmqtt::pub::MultiClientPublisher multi(UNSECURE_NO_AUTH_BROKER, "stress-multi", {.connections = NUM_CONNS });
            Assert::IsTrue(multi.Connect().ok(), L"Pub connect failed.");
            Assert::IsTrue(WaitForConnection(multi), L"Not connected.");

            // Single thread pushes TOTAL messages; multi spreads across connections internally.
            for (int i = 0; i < TOTAL; ++i)
              (void)multi.Publish(TOPIC, "payload", libmqtt::QoS::ExactlyOnce);

            // Wait for all
            for (int i = 0; i < 100 && recv.load() != TOTAL; ++i)
              std::this_thread::sleep_for(std::chrono::milliseconds(100));

            Assert::AreEqual(TOTAL, recv.load(), L"Listener did not receive all messages.");
            (void)multi.Disconnect(); sub.Disconnect();
          }

          TEST_METHOD(HighThroughput_Secure_SinglePublisher) {
            auto opts = libmqtt::ConnectionOptions{};
            opts.username = "testuser"; opts.password = "testpass";
            libmqtt::TlsOptions tls; tls.ca_file_path = GetCertPath("ca.crt");
            Assert::IsFalse(tls.ca_file_path.empty(), L"Missing CA cert.");
            opts.tls = tls;

            TestSubscriber sub(SECURE_WITH_AUTH_BROKER, "stress-secure-sub");
            std::atomic<int> recv{0};
            sub.SetMessageCallback([&](std::string_view, std::string_view) { recv.fetch_add(1, std::memory_order_acq_rel); });
            Assert::IsTrue(sub.Connect(opts).ok(), L"Sub connect failed."); sub.WaitConnected();

            const std::string TOPIC = "stress/secure/single";
            sub.Subscribe(TOPIC, 1);

            libmqtt::pub::PoolZeroPublisher pub(SECURE_WITH_AUTH_BROKER, "stress-secure-pub");
            Assert::IsTrue(pub.Connect(opts).ok(), L"Pub connect failed."); Assert::IsTrue(WaitForConnection(pub), L"Not connected.");

            const int ITERS = 500;
            std::thread t([&] {
              for (int i = 0; i < ITERS; ++i)
                (void)pub.Publish(TOPIC, "secure_message", libmqtt::QoS::ExactlyOnce);
            });
            t.join();

            for (int i = 0; i < 100 && recv.load() != ITERS; ++i)
              std::this_thread::sleep_for(std::chrono::milliseconds(100));

            Assert::AreEqual(ITERS, recv.load(), L"Not all secure messages received.");
            (void)pub.Disconnect(); sub.Disconnect();
          }

          TEST_METHOD(HighThroughput_Secure_MultiConn) {
            auto opts = libmqtt::ConnectionOptions{};
            opts.username = "testuser"; opts.password = "testpass";
            libmqtt::TlsOptions tls; tls.ca_file_path = GetCertPath("ca.crt");
            Assert::IsFalse(tls.ca_file_path.empty(), L"Missing CA cert.");
            opts.tls = tls;

            TestSubscriber sub(SECURE_WITH_AUTH_BROKER, "stress-secure-multi-sub");
            std::atomic<int> recv{0};
            sub.SetMessageCallback([&](std::string_view, std::string_view) { recv.fetch_add(1, std::memory_order_acq_rel); });
            Assert::IsTrue(sub.Connect(opts).ok(), L"Sub connect failed."); sub.WaitConnected();

            const std::string TOPIC = "stress/secure/multi";
            sub.Subscribe(TOPIC, 1);

            const int NUM_CONNS = 4;
            const int MSG_PER = 150;
            const int TOTAL = NUM_CONNS * MSG_PER;

            libmqtt::pub::MultiClientPublisher multi(SECURE_WITH_AUTH_BROKER, "stress-secure-multi", {.connections = NUM_CONNS });
            Assert::IsTrue(multi.Connect(opts).ok(), L"Pub connect failed.");
            Assert::IsTrue(WaitForConnection(multi), L"Not connected.");

            for (int i = 0; i < TOTAL; ++i)
              (void)multi.Publish(TOPIC, "secure_payload", libmqtt::QoS::ExactlyOnce);

            for (int i = 0; i < 120 && recv.load() != TOTAL; ++i)
              std::this_thread::sleep_for(std::chrono::milliseconds(100));

            Assert::AreEqual(TOTAL, recv.load(), L"Secure listener did not receive all messages.");
            (void)multi.Disconnect(); sub.Disconnect();
          }
    };
} // namespace UnitTest