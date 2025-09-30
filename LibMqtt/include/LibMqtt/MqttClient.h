#pragma once

/// @file MqttClient.h
/// @brief Public C++ interface for the LibMqtt library.
/// @note This header is intended to be binary-stable for consumers that link
///       against the produced library. Keep changes backwards-compatible.
#include <string>
#include <functional>
#include <memory>
#include <optional>
#include <cstdint>


/// @brief TLS-related options for a secure connection (optional).
struct TlsOptions {
    /// @brief Path to CA file used to validate the server certificate.
    std::string ca_file_path;
    /// @brief Path to client certificate (optional).
    std::string client_cert_path;
    /// @brief Path to client private key (optional).
    std::string client_key_path;
    /// @brief When true, validate the server certificate chain. Default: true.
    bool enable_server_cert_auth = true;
};

/// @brief Last Will & Testament (LWT) options.
/// @details The LWT is a message the broker will publish on behalf of the
///          client when the client disconnects unexpectedly. Useful to
///          signal liveness/state to other clients.
struct LastWill {
    /// @brief Topic to publish the will message to.
    std::string topic;
    /// @brief Payload/body for the will message (binary-safe string).
    std::string payload;
    /// @brief QoS level for the will (0, 1 or 2). Default: 1.
    int qos = 1;
    /// @brief Whether the will message should be retained by the broker.
    bool retained = false;
};

/// @brief Connection options including optional auth, TLS and LWT.
/// @note Pass this struct to Connect(...) when authentication, TLS, or an
///       LWT should be used.
struct ConnectionOptions {
    /// @brief Username for broker authentication.
    std::string username;
    /// @brief Password for broker authentication.
    std::string password;
    /// @brief Optional TLS parameters (set for secure connections).
    std::optional<TlsOptions> tls;
    /// @brief Optional Last Will to apply during Connect.
    std::optional<LastWill> last_will;
};

/// @brief Quality of Service levels for publish/subscribe.
enum class QoS : uint8_t {
    /// @brief QoS 0 (At most once).
    AtMostOnce = 0,
    /// @brief QoS 1 (At least once).
    AtLeastOnce = 1,
    /// @brief QoS 2 (Exactly once).
    ExactlyOnce = 2
};

/// @brief A simple, thread-safe MQTT client wrapper.
/// @remarks This class is a thin wrapper around the Paho MQTT C++ async client.
///          It provides a stable public API to be used by other teams.
class MqttClient {
public:
    /// @brief Message callback invoked when a message arrives.
    /// @param topic Topic of the incoming message.
    /// @param payload Message body as UTF-8 string.
    using MessageCallback = std::function<void(const std::string& topic, const std::string& payload)>;

    /// @brief Error callback invoked for connection/publish/subscribe errors.
    /// @param errorCode Numeric error code (Paho return code or -1 for library-level errors).
    /// @param errorMessage Human-readable message describing the error.
    using ErrorCallback = std::function<void(int errorCode, const std::string& errorMessage)>;

    /// @brief Callback invoked when an async publish completes.
    /// @param success true if publish was acknowledged (QoS>=1) or sent to queue (QoS0).
    /// @param rc return code (0 on success or mqtt/paho-specific code on failure).
    /// @param message optional status message.
    using PublishCallback = std::function<void(bool success, int rc, const std::string& message)>;

    /// @brief Construct a client with broker URI and client identifier.
    /// @param brokerAddress Broker URI, e.g. "tcp://host:1883" or "ssl://host:8883".
    /// @param clientId Unique client identifier.
    MqttClient(const std::string& brokerAddress, const std::string& clientId);

    /// @brief Construct from wide strings (convenience overload).
    /// @param brokerAddress Broker URI as wstring.
    /// @param clientId Client id as wstring.
    MqttClient(const std::wstring& brokerAddress, const std::wstring& clientId);

    /// @brief Destructor ensures a clean Disconnect() when possible.
    ~MqttClient();

    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;

    /// @brief Register message callback.
    /// @param callback Callable invoked for each incoming message.
    void SetCallback(MessageCallback callback);

    /// @brief Register error callback.
    /// @param callback Callable invoked on errors.
    void SetErrorCallback(ErrorCallback callback);

    /// @brief Connect without authentication or TLS.
    /// @note Uses library defaults (clean session and automatic reconnect).
    void Connect();

    /// @brief Connect with authentication/TLS/LWT options.
    /// @param options ConnectionOptions containing auth, TLS, and/or last will.
    void Connect(const ConnectionOptions& options);

    /// @brief Cleanly disconnect the client.
    void Disconnect();

    /// @brief Subscribe to a topic with specified QoS.
    /// @param topic Topic filter to subscribe to.
    /// @param qos QoS level for this subscription (default: QoS::AtMostOnce).
    void Subscribe(const std::string& topic, QoS qos = QoS::ExactlyOnce);

    /// @brief Wide-string overload for Subscribe.
    /// @param topic Topic filter as wstring.
    /// @param qos QoS level for this subscription.
    void Subscribe(const std::wstring& topic, QoS qos = QoS::ExactlyOnce);

    /// @brief Publish a message to a topic.
    /// @param topic Topic to publish to.
    /// @param payload Message body (binary-safe string).
    /// @param qos QoS level to use when publishing (default: QoS::AtLeastOnce).
    /// @param retained If true, instruct the broker to retain the last message for the topic.
    void Publish(const std::string& topic, const std::string& payload, QoS qos = QoS::ExactlyOnce, bool retained = false);

    /// @brief Wide-string overload for Publish.
    /// @param topic Topic as wstring.
    /// @param payload Payload as wstring.
    /// @param qos QoS level to use when publishing.
    /// @param retained Retain flag for the message.
    void Publish(const std::wstring& topic, const std::wstring& payload, QoS qos = QoS::ExactlyOnce, bool retained = false);

    /// @brief Set a Last Will that will be applied at the next Connect().
    /// @param lw LastWill struct describing topic/payload/qos/retained.
    void SetLastWill(const LastWill& lw);

    /// @brief Query whether the client is currently connected.
    /// @return true when connected to the broker; false otherwise.
    bool IsConnected() const;

private:
    class MqttClientImpl;
    std::unique_ptr<MqttClientImpl> m_impl;
};
