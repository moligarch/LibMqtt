#include "pch.h"
#include "CppUnitTest.h"

#include <atomic>
#include <string>
#include <mutex>
#include <thread>

// Make sure this path is correct for your project structure
#include "LibMqtt/MqttClient.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace MqttLibTests
{
	TEST_CLASS(ErrorHandlingTests)
	{
	public:

		TEST_METHOD(TestConnectionFailureCallback)
		{
			// Arrange: Set up client with a bad address and prepare to capture error info
			const std::string BAD_BROKER_ADDRESS = "tcp://localhost:1884";
			MqttClient client(BAD_BROKER_ADDRESS, "error-test-client");

			std::atomic<bool> errorCallbackWasCalled = false;
			std::atomic<int> capturedErrorCode = 0;
			std::string capturedErrorMessage;
			std::mutex messageMutex; // Protects the string

			auto errorCallback =
				[&](int errorCode, const std::string& errorMessage) {

				std::lock_guard<std::mutex> lock(messageMutex);
				capturedErrorCode = errorCode;
				capturedErrorMessage = errorMessage;
				errorCallbackWasCalled = true;
				};

			client.SetErrorCallback(errorCallback);

			// Act: Attempt to connect, which should fail and trigger the callback
			client.Connect();

			// Assert: Verify that the error callback was invoked with the expected info
			Assert::IsTrue(errorCallbackWasCalled.load(), L"The error callback was not called.");
			Assert::AreNotEqual(0, capturedErrorCode.load(), L"Error code should not be zero.");

			std::lock_guard<std::mutex> lock(messageMutex);
			Assert::IsTrue(capturedErrorMessage.find("TCP/TLS connect failure") != std::string::npos, L"Error message did not contain expected text.");
		}
	};

    TEST_CLASS(ThreadSafetyTests)
    {
    public:
        // Test 1: Verifies that the data path can handle high throughput
        // without dropping messages and without deadlocking.
        TEST_METHOD(HighThroughputDataTest)
        {
            // Arrange
            const std::string BROKER_ADDRESS = "tcp://localhost:1883";
            const std::string CLIENT_ID = "throughput-test-client";
            const std::string TOPIC = "throughput/test";
            MqttClient client(BROKER_ADDRESS, CLIENT_ID);

            std::atomic<int> received_count = 0;
            const int ITERS = 1000;

            client.SetCallback([&](const std::string&, const std::string&) {
                received_count++;
                });

            client.Connect();
            Assert::IsTrue(WaitForConnection(client), L"Client failed to connect for test.");
            client.Subscribe(TOPIC);

            // Act
            std::thread publisher([&]() {
                for (int i = 0; i < ITERS; ++i) {
                    client.Publish(TOPIC, "message");
                }
                });
            publisher.join();

            // Wait up to 2 seconds for all messages to be received by the callback.
            bool allMessagesReceived = false;
            for (int i = 0; i < 20 && !allMessagesReceived; ++i) {
                if (received_count.load() == ITERS) {
                    allMessagesReceived = true;
                }
                else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }

            // Assert
            Assert::AreEqual(ITERS, received_count.load(), L"Not all published messages were received.");
            client.Disconnect();
        }

        // Test 2: Verifies that it's safe to change callbacks while the
        // Paho background thread is simultaneously trying to use them.
        TEST_METHOD(ConfigurationSafetyTest)
        {
            // Arrange
            const std::string BROKER_ADDRESS = "tcp://localhost:1883";
            const std::string CLIENT_ID = "config-safety-test-client";
            const std::string TOPIC = "config/test";
            MqttClient client(BROKER_ADDRESS, CLIENT_ID);

            std::atomic<int> received_count = 0;
            const int ITERS = 500;

            client.SetCallback([&](const std::string&, const std::string&) {
                received_count++;
                });

            client.Connect();
            Assert::IsTrue(WaitForConnection(client), L"Client failed to connect for test.");
            client.Subscribe(TOPIC);

            // Act
            std::thread publisher([&]() {
                for (int i = 0; i < ITERS; ++i) {
                    client.Publish(TOPIC, "message");
                }
                });

            std::thread configurer([&]() {
                for (int i = 0; i < ITERS; ++i) {
                    // The lock in SetCallback prevents a crash while the Paho
                    // thread is trying to use the callback in message_arrived.
                    client.SetCallback([&](const std::string&, const std::string&) {
                        received_count++;
                        });
                }
                });

            publisher.join();
            configurer.join();
            client.Disconnect();

            // Assert: The primary test is that it completes without crashing.
            // We also check that at least some messages were processed.
            Assert::IsTrue(received_count.load() > 0, L"No messages were received during the test.");
        }

    private:
        // Helper function to wait for async connection
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
    };
}