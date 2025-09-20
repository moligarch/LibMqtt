#include <iostream>
#include <string>
#include <thread>
#include <chrono>

// Include the header for the library we just created.
#include "LibMqtt/MqttClient.h"

std::atomic<bool> _is_active{ true };

int main() {
    const std::string BROKER_ADDRESS = "tcp://localhost:1883";
    const std::string CLIENT_ID = "my-test-client-12345";
    const std::string TOPIC = "my/test/topic";

    std::cout << "--- MQTT Client Test Application ---" << std::endl;

    // 1. Instantiate the client
    MqttClient client(BROKER_ADDRESS, CLIENT_ID);

    // 2. Define the callback lambda function
    // This lambda will be executed whenever a message arrives.
    auto messageCallback = [](const std::string& topic, const std::string& payload) {
        if (payload == "exit") {
            _is_active.store(false);
            std::cout << "\n<-- Exit Message received!" << std::endl;
        }

        if (_is_active.load()) {
            std::cout << "\n<-- Message received!" << std::endl;
            std::cout << "  Topic: " << topic << std::endl;
            std::cout << "  Payload: " << payload << std::endl;
        }
        };

    // 3. Set the callback
    client.SetCallback(messageCallback);

    // 4. Connect to the broker
    client.Connect();

    // The connect call is asynchronous. For this simple test, we will wait a
    // moment to ensure the connection is established before proceeding.
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // 5. Subscribe to a topic
    // We only proceed if the connection was successful.
    if (client.IsConnected()) {
        client.Subscribe(TOPIC);

        // 6. Main loop to publish messages periodically
        int messageCount = 0;
        while (_is_active.load()) {
            std::string payload = "Hello from MqttLib! Message #" + std::to_string(++messageCount);

            std::cout << "--> Publishing: '" << payload << "' to topic '" << TOPIC << "'" << std::endl;
            client.Publish(TOPIC, payload);

            // Wait for 5 seconds before publishing the next message
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
    else {
        std::cerr << "Could not connect to the broker. Exiting." << std::endl;
    }

    return 0;
}