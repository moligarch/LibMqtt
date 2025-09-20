#include "pch.h"
#include "CppUnitTest.h"

#include <atomic>
#include <string>
#include <mutex>

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
}