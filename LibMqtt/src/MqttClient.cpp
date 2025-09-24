#include "LibMqtt/MqttClient.h"

#include <iostream>
#include <atomic>
#include <mutex>

// Paho MQTT C++ library headers
#include "mqtt/async_client.h"

#include "utils.h"

// This is the actual implementation class, hidden from the user.
// It inherits from Paho's callback classes to handle events.
class MqttClient::MqttClientImpl : public virtual mqtt::callback {
    std::mutex m_client_mutex;      // Protects Connect, Disconnect, Subscribe
    std::mutex m_callback_mutex;    // Protects the std::function callback objects
    mqtt::async_client m_client;
    MqttClient::MessageCallback m_userCallback;
    MqttClient::ErrorCallback m_errorCallback;
    std::atomic<bool> m_isConnected;

public:
    MqttClientImpl(const std::string& brokerAddress, const std::string& clientId)
        : m_client(brokerAddress, clientId), m_isConnected(false) {
        // Set this class as the callback handler for the Paho client.
        m_client.set_callback(*this);
    }

    // This is a Paho callback override. It is called when the client successfully
    // connects to the broker.
    void connected(const std::string& cause) override {
        std::cout << "--> Connection successful!" << std::endl;
        m_isConnected = true;
    }

    // This is a Paho callback override. It is called when the connection is lost.
    void connection_lost(const std::string& cause) override {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_isConnected = false;
        if (m_errorCallback && !cause.empty()) {
            m_errorCallback(-1, "Connection lost: " + cause);
        }
        else if (!cause.empty()) {
            std::cerr << "--> Connection lost: " << cause << std::endl;
        }
    }

    // This is the most important callback. It is called when a message arrives
    // on a topic that the client is subscribed to.
    void message_arrived(mqtt::const_message_ptr msg) override {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        if (m_userCallback) {
            m_userCallback(msg->get_topic(), msg->get_payload_str());
        }
    }

    void SetUserCallback(MessageCallback callback) {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_userCallback = callback;
    }

    void SetUserErrorCallback(ErrorCallback callback) {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_errorCallback = callback;
    }

    void Connect() {
        std::lock_guard<std::mutex> lock(m_client_mutex);
        if (IsConnected()) {
            return;
        }
        std::cout << "--> Connecting to broker..." << std::endl;
        auto connOpts = mqtt::connect_options_builder()
            .clean_session()
            .automatic_reconnect(std::chrono::seconds(2), std::chrono::seconds(30))
            .finalize();

        try {
            m_client.connect(connOpts)->wait();
        }
        catch (const mqtt::exception& exc) {
            std::lock_guard<std::mutex> cb_lock(m_callback_mutex);
            if (m_errorCallback) {
                m_errorCallback(exc.get_return_code(), exc.what());
            }
            else {
                std::cerr << "--> ERROR: Unable to connect to MQTT server: '"
                    << exc.what() << "'" << std::endl;
            }
        }
    }

    void Connect(const ConnectionOptions& options) {
        std::lock_guard<std::mutex> lock(m_client_mutex);
        if (IsConnected()) {
            return;
        }
        std::cout << "--> Connecting to broker..." << std::endl;
        auto connOptsBuilder = mqtt::connect_options_builder()
            .clean_session()
            .automatic_reconnect(std::chrono::seconds(2), std::chrono::seconds(30));

        // Set username and password if provided
        if (!options.username.empty()) {
            connOptsBuilder.user_name(options.username);
            connOptsBuilder.password(options.password);
        }

        // Set TLS options if provided
        if (options.tls.has_value()) {
            std::cout << "--> Using TLS for connection." << std::endl;
            const auto& tlsOptions = options.tls.value();
            auto sslopts = mqtt::ssl_options_builder()
                .trust_store(tlsOptions.ca_file_path)
                .key_store(tlsOptions.client_cert_path)
                .private_key(tlsOptions.client_key_path)
                .enable_server_cert_auth(tlsOptions.enable_server_cert_auth)
                .finalize();
            connOptsBuilder.ssl(sslopts);
        }

        try {
            m_client.connect(connOptsBuilder.finalize())->wait();
        }
        catch (const mqtt::exception& exc) {
            std::lock_guard<std::mutex> cb_lock(m_callback_mutex);
            if (m_errorCallback) {
                m_errorCallback(exc.get_return_code(), exc.what());
            }
            else {
                std::cerr << "--> ERROR: Unable to connect to MQTT server: '"
                    << exc.what() << "'" << std::endl;
            }
        }
    }

    void Disconnect() {
        std::lock_guard<std::mutex> lock(m_client_mutex);
        if (!IsConnected()) {
            return;
        }
        std::cout << "--> Disconnecting..." << std::endl;
        try {
            m_client.disconnect()->wait();
        }
        catch (const mqtt::exception& exc) {
            std::lock_guard<std::mutex> cb_lock(m_callback_mutex);
            if (m_errorCallback) {
                m_errorCallback(exc.get_return_code(), exc.what());
            }
            else {
                std::cerr << "--> ERROR during disconnect: " << exc.what() << std::endl;
            }
        }
        m_isConnected = false;
    }

    void Subscribe(const std::string& topic) {
        std::lock_guard<std::mutex> lock(m_client_mutex);
        if (!IsConnected()) {
            std::string errMsg = "Cannot subscribe, client is not connected.";
            std::lock_guard<std::mutex> cb_lock(m_callback_mutex);
            if (m_errorCallback) {
                m_errorCallback(-1, errMsg);
            }
            else {
                std::cerr << "--> " << errMsg << std::endl;
            }
            return;
        }
        std::cout << "--> Subscribing to topic '" << topic << "'" << std::endl;
        try {
            m_client.subscribe(topic, 1)->wait();
        }
        catch (const mqtt::exception& exc) {
            std::lock_guard<std::mutex> cb_lock(m_callback_mutex);
            if (m_errorCallback) {
                m_errorCallback(exc.get_return_code(), exc.what());
            }
            else {
                std::cerr << "--> ERROR during subscribe: " << exc.what() << std::endl;
            }
        }
    }

    void Publish(const std::string& topic, const std::string& payload) {
        if (!IsConnected()) {
            std::string errMsg = "Cannot publish, client is not connected.";
            std::lock_guard<std::mutex> cb_lock(m_callback_mutex);
            if (m_errorCallback) {
                m_errorCallback(-1, errMsg);
            }
            else {
                std::cerr << "--> " << errMsg << std::endl;
            }
            return;
        }
        try {
            auto msg = mqtt::make_message(topic, payload);
            msg->set_qos(1);
            m_client.publish(msg)->wait();
        }
        catch (const mqtt::exception& exc) {
            std::lock_guard<std::mutex> cb_lock(m_callback_mutex);
            if (m_errorCallback) {
                m_errorCallback(exc.get_return_code(), exc.what());
            }
            else {
                std::cerr << "--> ERROR during publish: " << exc.what() << std::endl;
            }
        }
    }

    bool IsConnected() const {
        return m_isConnected;
    }
};


// --- Implementation of the public MqttClient class ---

MqttClient::MqttClient(const std::string& brokerAddress, const std::string& clientId)
    : m_impl(std::make_unique<MqttClientImpl>(brokerAddress, clientId)) {
}

MqttClient::MqttClient(const std::wstring& brokerAddress, const std::wstring& clientId)
    : MqttClient(Utility::WstringToString(brokerAddress), Utility::WstringToString(clientId)) {
}

MqttClient::~MqttClient() {
    // The unique_ptr will be destroyed automatically, but we can ensure
    // a clean disconnect call.
    m_impl->Disconnect();
}

void MqttClient::SetCallback(MessageCallback callback) {
    m_impl->SetUserCallback(callback);
}

void MqttClient::SetErrorCallback(ErrorCallback callback) {
    m_impl->SetUserErrorCallback(callback);
}

void MqttClient::Connect() {
    m_impl->Connect();
}

void MqttClient::Connect(const ConnectionOptions& options) {
    m_impl->Connect(options);
}

void MqttClient::Disconnect() {
    m_impl->Disconnect();
}

void MqttClient::Subscribe(const std::string& topic) {
    m_impl->Subscribe(topic);
}

void MqttClient::Subscribe(const std::wstring& topic)
{
    m_impl->Subscribe(Utility::WstringToString(topic));
}

void MqttClient::Publish(const std::string& topic, const std::string& payload) {
    m_impl->Publish(topic, payload);
}

void MqttClient::Publish(const std::wstring& topic, const std::wstring& payload)
{
    m_impl->Publish(Utility::WstringToString(topic), Utility::WstringToString(payload));
}

bool MqttClient::IsConnected() const {
    return m_impl->IsConnected();
}