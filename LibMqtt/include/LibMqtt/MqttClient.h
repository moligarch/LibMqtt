#pragma once

#include <string>
#include <functional>
#include <memory>
#include <optional>


struct TlsOptions {
    // Path to the Certificate Authority (CA) certificate file.
    std::string ca_file_path;
    // Path to the client's certificate file.
    std::string client_cert_path;
    // Path to the client's private key file.
    std::string client_key_path;
    // If true, the client will verify the server's certificate.
    bool enable_server_cert_auth = true;
};

struct ConnectionOptions {
    std::string username;
    std::string password;
    // Use std::optional to indicate if TLS should be used.
    std::optional<TlsOptions> tls;
};



class MqttClient {
public:
    // Defines the signature for the message handling callback function.
    // It receives the topic and the message payload as strings.
    using MessageCallback = std::function<void(const std::string& topic, const std::string& payload)>;

    // Defines the signature for the error handling callback function.
    using ErrorCallback = std::function<void(int errorCode, const std::string& errorMessage)>;

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
     * @brief Sets the callback function that will be invoked when an error occurs.
     * @param callback The function to be called.
     */
    void SetErrorCallback(ErrorCallback callback);

    /**
     * @brief Connects to the MQTT broker asynchronously with security options.
     * @param options The connection options, including credentials and TLS settings.
     */
    void Connect(const ConnectionOptions& options);

    /**
     * @brief Connects to the MQTT broker asynchronously (unsecured).
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