#include "pch.h"
#include "CppUnitTest.h"
#include <atomic>
#include <string>
#include <mutex>
#include <thread>
#include <vector>
#include <filesystem>
#include "LibMqtt/MqttClient.h"
#include "utils.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace MqttLibTests
{
    // Broker endpoints from our docker-compose setup
    const char* UNSECURE_NO_AUTH_BROKER = "mqtt://localhost:1883";
    const char* UNSECURE_WITH_AUTH_BROKER = "mqtt://localhost:1882";
    const char* SECURE_WITH_AUTH_BROKER = "mqtts://localhost:8883";
    const char* SECURE_NO_AUTH_BROKER = "mqtts://localhost:8884";

    // --- Base class with test utilities to avoid code duplication ---
    class TestBase
    {
    public:
        // Helper function to wait for an async connection to complete
        bool WaitForConnection(MqttClient& client, int timeout_sec = 5)
        {
            for (int i = 0; i < timeout_sec * 10; ++i) {
                if (client.IsConnected()) {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            return false;
        }

        // Helper to find the certificate file relative to the project structure
        std::string GetCertPath(const std::string& certName)
        {
            // Note: This relative path is fragile. A better solution involves
            // a post-build event to copy certs to the output directory.
            // Adjust the number of "../" if your build output location is different.
            auto path = "../../../../mosquitto/certs/" + certName;
            if (!std::filesystem::exists(path)) {
                Logger::WriteMessage(("Certificate not found at: " + path).c_str());
                return "";
            }
            return path;
        }
    };

    //#################################################################################
    // TEST CATEGORY: CONNECTIVITY
    // Description: These tests verify basic connect, publish, and subscribe
    // functionality against each of the four configured broker endpoints.
    //#################################################################################
    TEST_CLASS(ConnectivityTests), public TestBase
    {
    public:
        TEST_METHOD(Unsecure_NoAuth_ConnectPublishSubscribe)
        {
            TestPubSub(UNSECURE_NO_AUTH_BROKER, "conn-unsecure-noauth", ConnectionOptions{});
        }

        TEST_METHOD(Unsecure_WithAuth_ConnectPublishSubscribe)
        {
            ConnectionOptions opts;
            opts.username = "testuser";
            opts.password = "testpass";
            TestPubSub(UNSECURE_WITH_AUTH_BROKER, "conn-unsecure-auth", opts);
        }

        TEST_METHOD(Secure_NoAuth_ConnectPublishSubscribe)
        {
            ConnectionOptions opts;
            TlsOptions tls;
            tls.ca_file_path = GetCertPath("ca.crt");
            Assert::IsFalse(tls.ca_file_path.empty(), L"CA certificate not found for test.");
            opts.tls = tls;
            TestPubSub(SECURE_NO_AUTH_BROKER, "conn-secure-noauth", opts);
        }

        TEST_METHOD(Secure_WithAuth_ConnectPublishSubscribe)
        {
            ConnectionOptions opts;
            opts.username = "testuser";
            opts.password = "testpass";
            TlsOptions tls;
            tls.ca_file_path = GetCertPath("ca.crt");
            Assert::IsFalse(tls.ca_file_path.empty(), L"CA certificate not found for test.");
            opts.tls = tls;
            TestPubSub(SECURE_WITH_AUTH_BROKER, "conn-secure-auth", opts);
        }

        TEST_METHOD(WString_Overload_ConnectPublishSubscribe)
{
            // Arrange
            const std::wstring BROKER = L"tcp://localhost:1883";
            const std::wstring CLIENT_ID = L"conn-wstring-overload";
            MqttClient client(BROKER, CLIENT_ID); // Using wstring constructor

            std::atomic<bool> messageReceived = false;
            std::string receivedPayload; // Callback provides std::string
            std::mutex mtx;

            client.SetCallback([&](const std::string& topic, const std::string& payload) {
                std::lock_guard<std::mutex> lock(mtx);
                receivedPayload = payload;
                messageReceived = true;
            });

            // Act
            client.Connect();
            Assert::IsTrue(WaitForConnection(client), L"Client failed to connect using wstring overload.");

            const std::wstring TOPIC = L"connectivity/wstring";
            const std::wstring MESSAGE = L"hello-wstring";

            client.Subscribe(TOPIC); // Using wstring subscribe
            client.Publish(TOPIC, MESSAGE); // Using wstring publish

            // Wait for message
            for (int i = 0; i < 20 && !messageReceived; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            // Assert
            Assert::IsTrue(messageReceived.load(), L"Did not receive published wstring message.");
            // The received payload is a UTF-8 string, so we encode our wstring message for comparison
            Assert::AreEqual(Utility::WstringToString(MESSAGE), receivedPayload, L"Payload did not match sent wstring message.");
            client.Disconnect();
        }

    private:
        void TestPubSub(const std::string & broker, const std::string & clientId, const ConnectionOptions & opts)
        {
            // Arrange
            MqttClient client(broker, clientId);
            std::atomic<bool> messageReceived = false;
            std::string receivedPayload;
            std::mutex mtx;

            client.SetCallback([&](const std::string& topic, const std::string& payload) {
                std::lock_guard<std::mutex> lock(mtx);
                receivedPayload = payload;
                messageReceived = true;
            });

            // Act
            client.Connect(opts);
            Assert::IsTrue(WaitForConnection(client), (L"Client failed to connect to " + std::wstring(broker.begin(), broker.end())).c_str());

            const std::string TOPIC = "connectivity/test";
            const std::string MESSAGE = "hello-" + clientId;
            client.Subscribe(TOPIC);
            client.Publish(TOPIC, MESSAGE);

            // Wait for message
            for (int i = 0; i < 20 && !messageReceived; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            // Assert
            Assert::IsTrue(messageReceived.load(), L"Did not receive published message.");
            Assert::AreEqual(MESSAGE, receivedPayload, L"Payload did not match sent message.");
            client.Disconnect();
        }
    };

    //#################################################################################
    // TEST CATEGORY: ERROR HANDLING
    // Description: These tests ensure the client behaves predictably when
    // encountering connection errors or API misuse.
    //#################################################################################
    TEST_CLASS(ErrorHandlingTests), public TestBase
    {
    public:
        TEST_METHOD(ConnectionFailure_BadPort)
        {
            const std::string BAD_BROKER = "tcp://localhost:9999";
            MqttClient client(BAD_BROKER, "error-bad-port");
            std::atomic<bool> errorCallbackCalled = false;
            client.SetErrorCallback([&](int, const std::string&) { errorCallbackCalled = true; });

            client.Connect(); // This is async, error happens on background thread

            // Give time for the connection attempt to fail
            std::this_thread::sleep_for(std::chrono::seconds(2));

            Assert::IsFalse(client.IsConnected(), L"Client should not be connected.");
            Assert::IsTrue(errorCallbackCalled.load(), L"Error callback was not invoked on connection failure.");
        }

        TEST_METHOD(ConnectionFailure_BadCredentials)
        {
            MqttClient client(UNSECURE_WITH_AUTH_BROKER, "error-bad-creds");
            std::atomic<bool> errorCallbackCalled = false;

            ConnectionOptions opts;
            opts.username = "testuser";
            opts.password = "wrongpassword"; // Incorrect password

            client.SetErrorCallback([&](int, const std::string&) { errorCallbackCalled = true; });
            client.Connect(opts);

            std::this_thread::sleep_for(std::chrono::seconds(2));

            Assert::IsFalse(client.IsConnected(), L"Client should not be connected with bad credentials.");
            Assert::IsTrue(errorCallbackCalled.load(), L"Error callback was not invoked on auth failure.");
        }

        TEST_METHOD(Publish_WithoutConnection_FailsGracefully)
        {
            MqttClient client(UNSECURE_NO_AUTH_BROKER, "error-no-connect");
            std::atomic<bool> errorCallbackCalled = false;

            client.SetErrorCallback([&](int, const std::string& msg) {
                if (msg.find("not connected") != std::string::npos) {
                    errorCallbackCalled = true;
                }
            });

            // Act: Publish without connecting. This should not crash.
            client.Publish("some/topic", "some/payload");

            Assert::IsTrue(errorCallbackCalled.load(), L"Error callback for not being connected was not called.");
        }
    };

    //#################################################################################
    // TEST CATEGORY: RACE CONDITIONS & THREAD SAFETY
    // Description: These tests create multiple threads to access client
    // functions concurrently, probing for potential deadlocks or data corruption.
    //#################################################################################
    TEST_CLASS(RaceConditionTests), public TestBase
    {
    public:
        TEST_METHOD(ConcurrentSetCallback)
        {
            // Arrange
            MqttClient client(UNSECURE_NO_AUTH_BROKER, "race-set-callback");
            std::atomic<int> received_count = 0;
            const int ITERS = 500;
            client.SetCallback([&](const std::string&, const std::string&) { received_count++; });
            client.Connect();
            Assert::IsTrue(WaitForConnection(client), L"Client failed to connect for test.");

            const std::string TOPIC = "race/callback";
            client.Subscribe(TOPIC);
            std::this_thread::sleep_for(std::chrono::milliseconds(200)); // allow subscribe to complete

            // Act
            std::thread publisher([&]() {
                for (int i = 0; i < ITERS; ++i) { client.Publish(TOPIC, "message"); }
            });
            std::thread configurer([&]() {
                for (int i = 0; i < ITERS; ++i) {
                    // The mutex in SetCallback should prevent a data race with the
                    // Paho thread trying to invoke the callback in message_arrived.
                    client.SetCallback([&](const std::string&, const std::string&) { received_count++; });
                }
            });

            publisher.join();
            configurer.join();

            // Assert: The primary goal is that this completes without crashing.
            // We also check that messages were processed.
            // A short wait to allow the last messages to arrive.
            std::this_thread::sleep_for(std::chrono::seconds(1));
            Assert::IsTrue(received_count.load() > 0, L"No messages were received during the test.");
            client.Disconnect();
        }

        TEST_METHOD(ConnectionStateResilienceTest)
        {
            // This test ensures the client remains stable when one thread manages
            // the connection while another tries to use it.
            MqttClient client(UNSECURE_NO_AUTH_BROKER, "resilience-test");
            std::atomic<bool> stop_signal = false;

            // Thread 1: The connection manager. Repeatedly connects and disconnects.
            std::thread connection_manager([&]() {
                for (int i = 0; i < 10; ++i) {
                    if (stop_signal) break;
                    client.Connect();
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    if (!client.IsConnected()) {
                        // If connect fails, just try again next iteration.
                        // This might happen if the publisher thread is hammering publish
                        // while the client is in a transitional state.
                        continue;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    client.Disconnect();
                }
                stop_signal = true;
            });

            // Thread 2: The user. Repeatedly tries to publish.
            std::thread publisher([&]() {
                while (!stop_signal) {
                    // This call should succeed sometimes and fail gracefully others,
                    // but it should never crash or deadlock.
                    client.Publish("resilience/topic", "data");
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            });

            connection_manager.join();
            publisher.join();

            // Assert: The test passes if it completes without crashing or deadlocking.
            Assert::IsFalse(client.IsConnected(), L"Client should end in a disconnected state.");
        }
    };


    //#################################################################################
    // TEST CATEGORY: STRESS TESTS
    // Description: These tests push the client's performance limits with
    // high message throughput from one or more clients.
    //#################################################################################
    TEST_CLASS(StressTests), public TestBase
    {
    public:
        TEST_METHOD(HighThroughput_SinglePublisher)
        {
            // Arrange
            MqttClient client(UNSECURE_NO_AUTH_BROKER, "stress-single-pub");
            std::atomic<int> received_count = 0;
            const int ITERS = 1000;
            client.SetCallback([&](const std::string&, const std::string&) { received_count++; });
            client.Connect();
            Assert::IsTrue(WaitForConnection(client), L"Client failed to connect for test.");

            const std::string TOPIC = "stress/single";
            client.Subscribe(TOPIC);
            std::this_thread::sleep_for(std::chrono::milliseconds(200)); // allow subscribe to complete

            // Act
            std::thread publisher([&]() {
                for (int i = 0; i < ITERS; ++i) { client.Publish(TOPIC, "message"); }
            });
            publisher.join();

            // Assert
            bool allMessagesReceived = false;
            for (int i = 0; i < 40 && !allMessagesReceived; ++i) { // Increased wait time
                if (received_count.load() == ITERS) { allMessagesReceived = true; }
                else { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
            }
            Assert::AreEqual(ITERS, received_count.load(), L"Not all published messages were received.");
            client.Disconnect();
        }

        TEST_METHOD(HighThroughput_MultiplePublishers)
        {
            // Arrange
            const std::string TOPIC = "stress/multi";
            const int NUM_PUBLISHERS = 5;
            const int MSG_PER_PUBLISHER = 200;
            const int TOTAL_MESSAGES = NUM_PUBLISHERS * MSG_PER_PUBLISHER;

            // Create a dedicated listener client
            MqttClient listener(UNSECURE_NO_AUTH_BROKER, "stress-multi-listener");
            std::atomic<int> received_count = 0;
            listener.SetCallback([&](const std::string&, const std::string&) { received_count++; });
            listener.Connect();
            Assert::IsTrue(WaitForConnection(listener), L"Listener client failed to connect.");
            listener.Subscribe(TOPIC);
            std::this_thread::sleep_for(std::chrono::milliseconds(500)); // Allow subscribe to propagate

            // Act: Create and run multiple publisher clients in parallel
            std::vector<std::thread> publisher_threads;
            std::vector<std::unique_ptr<MqttClient>> publishers;
            for (int i = 0; i < NUM_PUBLISHERS; ++i) {
                std::string clientId = "stress-multi-pub-" + std::to_string(i);
                publishers.push_back(std::make_unique<MqttClient>(UNSECURE_NO_AUTH_BROKER, clientId));
                auto& pub_client = *publishers.back();

                publisher_threads.emplace_back([&]() {
                    pub_client.Connect();
                    if (WaitForConnection(pub_client, 2)) {
                        for (int j = 0; j < MSG_PER_PUBLISHER; ++j) {
                            pub_client.Publish(TOPIC, "payload");
                        }
                    }
                    else {
                        Assert::Fail(L"A publisher client failed to connect.");
                    }
                    pub_client.Disconnect();
                });
            }

            for (auto& t : publisher_threads) {
                t.join();
            }

            // Assert
            bool allMessagesReceived = false;
            for (int i = 0; i < 50 && !allMessagesReceived; ++i) {
                if (received_count.load() == TOTAL_MESSAGES) { allMessagesReceived = true; }
                else { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
            }
            Assert::AreEqual(TOTAL_MESSAGES, received_count.load(), L"Listener did not receive all messages from multiple publishers.");
            listener.Disconnect();
        }

        TEST_METHOD(HighThroughput_Secure_SinglePublisher)
        {
            // Arrange
            // Use the secure, authenticated broker
            MqttClient client(SECURE_WITH_AUTH_BROKER, "stress-secure-single-pub");
            std::atomic<int> received_count = 0;
            const int ITERS = 500; // Reduced iterations slightly for TLS overhead

            // Set up secure connection options
            ConnectionOptions opts;
            opts.username = "testuser";
            opts.password = "testpass";
            TlsOptions tls;
            tls.ca_file_path = GetCertPath("ca.crt");
            Assert::IsFalse(tls.ca_file_path.empty(), L"CA certificate not found for test.");
            opts.tls = tls;

            client.SetCallback([&](const std::string&, const std::string&) { received_count++; });
            client.Connect(opts);
            Assert::IsTrue(WaitForConnection(client), L"Client failed to connect securely for test.");

            const std::string TOPIC = "stress/secure/single";
            client.Subscribe(TOPIC);
            std::this_thread::sleep_for(std::chrono::milliseconds(200)); // allow subscribe to complete

            // Act
            std::thread publisher([&]() {
                for (int i = 0; i < ITERS; ++i) { client.Publish(TOPIC, "secure_message"); }
            });
            publisher.join();

            // Assert
            bool allMessagesReceived = false;
            for (int i = 0; i < 40 && !allMessagesReceived; ++i) {
                if (received_count.load() == ITERS) { allMessagesReceived = true; }
                else { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
            }
            Assert::AreEqual(ITERS, received_count.load(), L"Not all published messages were received over secure connection.");
            client.Disconnect();
        }

        TEST_METHOD(HighThroughput_Secure_MultiplePublishers)
        {
            // Arrange
            const std::string TOPIC = "stress/secure/multi";
            const int NUM_PUBLISHERS = 4;
            const int MSG_PER_PUBLISHER = 150;
            const int TOTAL_MESSAGES = NUM_PUBLISHERS * MSG_PER_PUBLISHER;

            // Set up secure connection options for all clients
            ConnectionOptions opts;
            opts.username = "testuser";
            opts.password = "testpass";
            TlsOptions tls;
            tls.ca_file_path = GetCertPath("ca.crt");
            Assert::IsFalse(tls.ca_file_path.empty(), L"CA certificate not found for test.");
            opts.tls = tls;

            // Create a dedicated listener client
            MqttClient listener(SECURE_WITH_AUTH_BROKER, "stress-secure-multi-listener");
            std::atomic<int> received_count = 0;
            listener.SetCallback([&](const std::string&, const std::string&) { received_count++; });
            listener.Connect(opts);
            Assert::IsTrue(WaitForConnection(listener), L"Secure listener client failed to connect.");
            listener.Subscribe(TOPIC);
            std::this_thread::sleep_for(std::chrono::milliseconds(500)); // Allow subscribe to propagate

            // Act: Create and run multiple secure publisher clients in parallel
            std::vector<std::thread> publisher_threads;
            std::vector<std::unique_ptr<MqttClient>> publishers;
            for (int i = 0; i < NUM_PUBLISHERS; ++i) {
                std::string clientId = "stress-secure-multi-pub-" + std::to_string(i);
                publishers.push_back(std::make_unique<MqttClient>(SECURE_WITH_AUTH_BROKER, clientId));
                auto& pub_client = *publishers.back();

                publisher_threads.emplace_back([&, opts]() { // Pass opts by value
                    pub_client.Connect(opts);
                    if (WaitForConnection(pub_client, 3)) {
                        for (int j = 0; j < MSG_PER_PUBLISHER; ++j) {
                            pub_client.Publish(TOPIC, "secure_payload");
                        }
                    }
                    else {
                        Assert::Fail(L"A secure publisher client failed to connect.");
                    }
                    pub_client.Disconnect();
                });
            }

            for (auto& t : publisher_threads) {
                t.join();
            }

            // Assert
            bool allMessagesReceived = false;
            for (int i = 0; i < 50 && !allMessagesReceived; ++i) {
                if (received_count.load() == TOTAL_MESSAGES) { allMessagesReceived = true; }
                else { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
            }
            Assert::AreEqual(TOTAL_MESSAGES, received_count.load(), L"Secure listener did not receive all messages.");
            listener.Disconnect();
        }
    };
}