#include "LibMqtt/MqttClient.h"
#include <iostream>
#include <atomic>
#include <mutex>
#include "mqtt/async_client.h"
#include "utils.h"

class MqttClient::MqttClientImpl : public virtual mqtt::callback {
    std::mutex m_client_mutex;
    std::mutex m_callback_mutex;
    mqtt::async_client m_client;
    MqttClient::MessageCallback m_userCallback;
    MqttClient::ErrorCallback m_errorCallback;
    std::atomic<bool> m_isConnected;
    std::optional<LastWill> m_lastWill; // stored to apply at connect time
public:
    MqttClientImpl(const std::string& brokerAddress, const std::string& clientId)
        : m_client(brokerAddress, clientId), m_isConnected(false) {
        m_client.set_callback(*this);
    }

    // --- Paho callbacks ---
    void connected(const std::string& cause) override {
        std::cout << "--> Connection successful!" << std::endl;
        m_isConnected = true;
    }
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
    void message_arrived(mqtt::const_message_ptr msg) override {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        if (m_userCallback) {
            m_userCallback(msg->get_topic(), msg->get_payload_str());
        }
    }

    // --- user facing setters ---
    void SetUserCallback(MessageCallback callback) {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_userCallback = callback;
    }
    void SetUserErrorCallback(ErrorCallback callback) {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        m_errorCallback = callback;
    }

    void SetLastWill(const LastWill& lw) {
        std::lock_guard<std::mutex> lock(m_client_mutex);
        m_lastWill = lw;
    }

    // --- connect variants ---
    void Connect() {
        std::lock_guard<std::mutex> lock(m_client_mutex);
        if (IsConnected()) return;
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

        if (!options.username.empty()) {
            connOptsBuilder.user_name(options.username);
            connOptsBuilder.password(options.password);
        }

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

        // Apply LWT - prefer the explicit ConnectionOptions.last_will if present,
        // otherwise use m_lastWill set by SetLastWill().
        std::optional<LastWill> willSource;
        if (options.last_will.has_value()) willSource = options.last_will;
        else if (m_lastWill.has_value()) willSource = m_lastWill;

        if (willSource.has_value()) {
            const auto& lw = willSource.value();
            std::cout << "--> Configuring Last Will: topic='" << lw.topic << "'" << std::endl;
            // create a will_options object (Paho C++ API)
            mqtt::will_options willOpts(
                lw.topic,
                lw.payload.data(),
                lw.payload.size(),
                static_cast<int>(lw.qos),
                lw.retained
            );
            connOptsBuilder.will(willOpts);
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

    void Subscribe(const std::string& topic, QoS qos) {
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
        std::cout << "--> Subscribing to topic '" << topic << "' qos=" << static_cast<int>(qos) << std::endl;
        try {
            m_client.subscribe(topic, static_cast<int>(qos))->wait();
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

    void Publish(const std::string& topic, const std::string& payload, QoS qos, bool retained) {
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
            msg->set_qos(static_cast<int>(qos));
            msg->set_retained(retained);
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

// ---- wrapper methods ----

MqttClient::MqttClient(const std::string& brokerAddress, const std::string& clientId)
    : m_impl(std::make_unique<MqttClientImpl>(brokerAddress, clientId)) {
}

MqttClient::MqttClient(const std::wstring& brokerAddress, const std::wstring& clientId)
    : MqttClient(Utility::WstringToString(brokerAddress), Utility::WstringToString(clientId)) {
}

MqttClient::~MqttClient() {
    if (m_impl) m_impl->Disconnect();
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

void MqttClient::Subscribe(const std::string& topic, QoS qos) {
    m_impl->Subscribe(topic, qos);
}

void MqttClient::Subscribe(const std::wstring& topic, QoS qos) {
    m_impl->Subscribe(Utility::WstringToString(topic), qos);
}

void MqttClient::Publish(const std::string& topic, const std::string& payload, QoS qos, bool retained) {
    m_impl->Publish(topic, payload, qos, retained);
}

void MqttClient::Publish(const std::wstring& topic, const std::wstring& payload, QoS qos, bool retained) {
    m_impl->Publish(Utility::WstringToString(topic), Utility::WstringToString(payload), qos, retained);
}

void MqttClient::SetLastWill(const LastWill& lw) {
    m_impl->SetLastWill(lw);
}

bool MqttClient::IsConnected() const {
    return m_impl->IsConnected();
}