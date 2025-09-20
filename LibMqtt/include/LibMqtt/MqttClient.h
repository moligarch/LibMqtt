#pragma once

#include <string>
#include <functional>
#include <memory>

class MqttClient {
public:
    // Defines the signature for the message handling callback function.
    // It receives the topic and the message payload as strings.
    using MessageCallback = std::function<void(const std::string& topic, const std::string& payload)>;

    /**
     * @brief Constructs the MqttClient.
     * @param brokerAddress The full address of the broker (e.g., "tcp://localhost:1883").
     * @param clientId A unique identifier for this client.
     */
    MqttClient(const std::string& brokerAddress, const std::string& clientId);

    /**
     * @brief Destructor. Ensures the client is disconnected cleanly.
     */
    ~MqttClient();

    // Disabling copy and move constructors for simplicity, as the underlying
    // client has a unique connection.
    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;

    /**
     * @brief Sets the callback function that will be invoked when a message arrives.
     * @param callback The function to be called.
     */
    void SetCallback(MessageCallback callback);

    /**
     * @brief Connects to the MQTT broker asynchronously.
     */
    void Connect();

    /**
     * @brief Disconnects from the MQTT broker.
     */
    void Disconnect();

    /**
     * @brief Subscribes to a topic filter.
     * @param topic The topic to subscribe to.
     */
    void Subscribe(const std::string& topic);

    /**
     * @brief Publishes a message to a topic.
     * @param topic The topic to publish the message to.
     * @param payload The content of the message.
     */
    void Publish(const std::string& topic, const std::string& payload);

    /**
     * @brief Checks if the client is currently connected to the broker.
     * @return True if connected, false otherwise.
     */
    bool IsConnected() const;

private:
    class MqttClientImpl;
    std::unique_ptr<MqttClientImpl> m_impl;
};