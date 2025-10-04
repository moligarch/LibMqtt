#pragma once
/**
 * @file OptionsBuilder.h
 * @brief Single source of truth for Paho connect options (TLS, creds, reconnect).
 */

#include <optional>
#include <mqtt/connect_options.h>
#include <mqtt/ssl_options.h>
#include "LibMqtt/Core/Types.h"

namespace libmqtt {

    /// Build Paho connect options with automatic reconnect enabled.
    inline mqtt::connect_options make_connect_options(const std::optional<ConnectionOptions>& opts) {
        auto b = mqtt::connect_options_builder().clean_session(false).automatic_reconnect(true);
        if (opts.has_value()) {
            const auto& o = opts.value();
            if (!o.username.empty()) { b.user_name(o.username); b.password(o.password); }
            if (o.tls.has_value()) {
                mqtt::ssl_options ssl;
                const auto& t = o.tls.value();
                if (!t.ca_file_path.empty()) ssl.set_trust_store(t.ca_file_path.string());
                ssl.set_enable_server_cert_auth(t.enable_server_cert_auth);
                b.ssl(ssl);
            }
        }
        return b.finalize();
    }

}  // namespace libmqtt