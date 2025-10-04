#pragma once
#include <optional>
#include <string>
#include <filesystem>

namespace libmqtt {

    enum class QoS { 
        AtMostOnce = 0,
        AtLeastOnce = 1, 
        ExactlyOnce = 2 
    };

    // Non-throwing status code for API returns
    enum class ResultCode {
        Ok, 
        Timeout,
        Disconnected, 
        ProtocolError, 
        Cancelled, 
        Unknown 
    };

    struct Status {
        ResultCode code{ ResultCode::Ok };
        constexpr bool ok() const noexcept { return code == ResultCode::Ok; }
    };

    inline constexpr const char* ToString(ResultCode c) noexcept {
        switch (c) {
        case ResultCode::Ok:           return "Ok";
        case ResultCode::Timeout:      return "Timeout";
        case ResultCode::Disconnected: return "Disconnected";
        case ResultCode::ProtocolError:return "ProtocolError";
        case ResultCode::Cancelled:    return "Cancelled";
        default:                       return "Unknown";
        }
    }

    struct LastWill {
        std::string topic;
        std::string payload;
        QoS         qos{ QoS::AtLeastOnce };
        bool        retained{ false };
    };

    struct TlsOptions {
        std::filesystem::path ca_file_path;       // CA trust store (PEM)
        std::filesystem::path client_cert_path;   // Client cert (optional)
        std::filesystem::path client_key_path;    // Client key (optional)
        bool                  enable_server_cert_auth{ true };
    };

    struct ConnectionOptions {
        std::string                 username;
        std::string                 password;
        std::optional<TlsOptions>   tls;
        std::optional<LastWill>     last_will;
    };

} // namespace libmqtt
