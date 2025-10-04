#include "LibMqtt/Core/Connect.h"
#include <string>
#include "mqtt/async_client.h"
#include "LibMqtt/Core/Logging.h"

namespace libmqtt {

    static inline std::string path_str(const std::filesystem::path& p) {
        return p.empty() ? std::string{} : p.string();
    }

    Status ConnectBlocking(mqtt::async_client& client,
        const std::optional<ConnectionOptions>& opts,
        std::chrono::milliseconds timeout) noexcept
    {
        try {
            auto b = mqtt::connect_options_builder()
                .clean_session()
                .automatic_reconnect(false);

            if (opts.has_value()) {
                const auto& o = *opts;
                if (!o.username.empty()) { b.user_name(o.username); b.password(o.password); }
                if (o.tls.has_value()) {
                    const auto& t = *o.tls;
                    mqtt::ssl_options ssl;
                    const std::string ca = path_str(t.ca_file_path);
                    const std::string cert = path_str(t.client_cert_path);
                    const std::string key = path_str(t.client_key_path);
                    if (!ca.empty())   ssl.set_trust_store(ca);
                    if (!cert.empty()) ssl.set_key_store(cert);
                    if (!key.empty())  ssl.set_private_key(key);
                    ssl.set_enable_server_cert_auth(t.enable_server_cert_auth);
                    b.ssl(ssl);
                }
                if (o.last_will.has_value()) {
                    const auto& lw = *o.last_will;
                    mqtt::will_options w(lw.topic, lw.payload.data(), lw.payload.size(),
                        static_cast<int>(lw.qos), lw.retained);
                    b.will(w);
                }
            }

            auto tok = client.connect(b.finalize());
            if (!tok->wait_for(timeout)) {
                LIBMQTT_LOG(LogLevel::Error, "ConnectBlocking: timeout waiting for CONNACK");
                return { ResultCode::Timeout };
            }
            if (!client.is_connected()) {
                LIBMQTT_LOG(LogLevel::Error, "ConnectBlocking: not connected after wait");
                return { ResultCode::ProtocolError };
            }
            return { ResultCode::Ok };
        }
        catch (const mqtt::exception& e) {
            // Contain exceptions here and map to a status
            LIBMQTT_LOG(LogLevel::Error, std::string("ConnectBlocking: exception: ") + e.what());
            // We could map reason codes more finely, but keep it simple & non-throwing:
            return { ResultCode::ProtocolError };
        }
        catch (...) {
            LIBMQTT_LOG(LogLevel::Error, "ConnectBlocking: unknown exception");
            return { ResultCode::Unknown };
        }
    }

    Status DisconnectQuiet(mqtt::async_client& client) noexcept {
        try { if (client.is_connected()) client.disconnect()->wait(); }
        catch (...) {}
        return { ResultCode::Ok };
    }

} // namespace libmqtt
