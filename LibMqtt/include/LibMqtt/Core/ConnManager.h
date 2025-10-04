#pragma once
/**
 * @file ConnManager.h
 * @brief Owns a Paho async_client and provides event-driven connection state.
 */

#include <optional>
#include <string>
#include <mqtt/async_client.h>
#include "LibMqtt/Core/ConnectedLatch.h"
#include "LibMqtt/Core/OptionsBuilder.h"
#include "LibMqtt/Core/Types.h"
#include "LibMqtt/Core/Logging.h"

namespace libmqtt {

    /**
     * @brief Thin wrapper around mqtt::async_client that
     *  - wires connected()/connection_lost() to a ConnectedLatch
     *  - funnels errors to the user error callback
     *  - provides Connect/Disconnect that wait on Paho tokens
     */
    class ConnManager : public virtual mqtt::callback {
    public:
        ConnManager(std::string broker_uri,
            std::string client_id,
            CallbackRCU<ErrorCallback>* err_cb_ptr)
            : cli_(std::move(broker_uri), std::move(client_id)), err_cb_ptr_(err_cb_ptr) {
            cli_.set_callback(*this);
        }

        ConnManager(const ConnManager&) = delete;
        ConnManager& operator=(const ConnManager&) = delete;

        // mqtt::callback
        void connected(const std::string&) override {
            latch_.set_connected(true);
            LIBMQTT_LOG(LogLevel::Info, "ConnManager: connected");
        }
        void connection_lost(const std::string& cause) override {
            latch_.set_connected(false);
            if (err_cb_ptr_) { 
                if (auto cb = err_cb_ptr_->get()) {
                    (*cb)(ResultCode::Disconnected, cause);
                }
            }
            LIBMQTT_LOG(LogLevel::Warn, "ConnManager: connection lost");
        }
        void message_arrived(mqtt::const_message_ptr) override {}
        void delivery_complete(mqtt::delivery_token_ptr) override {}

        Status Connect(const std::optional<ConnectionOptions>& opts) noexcept {
            try {
                cli_.connect(make_connect_options(opts))->wait();
                latch_.set_connected(true);
                return Status{ ResultCode::Ok };
            }
            catch (const mqtt::exception& e) {
                if (err_cb_ptr_) { 
                    if (auto cb = err_cb_ptr_->get()) {
                        (*cb)(ResultCode::Disconnected, e.what());
                    }
                }
                return Status{ ResultCode::Disconnected };
            }
            catch (...) {
                if (err_cb_ptr_) { 
                    if (auto cb = err_cb_ptr_->get()) {
                        (*cb)(ResultCode::Unknown, "connect error");
                    }
                }
                return Status{ ResultCode::Unknown };
            }
        }

        Status Disconnect() noexcept {
            try { 
                if (cli_.is_connected()) {
                    cli_.disconnect()->wait();
                }
            }
            catch (...) {}

            latch_.set_connected(false);
            return Status{ ResultCode::Ok };
        }

        mqtt::async_client& client() noexcept { return cli_; }
        ConnectedLatch&     latch()  noexcept { return latch_; }

    private:
        mqtt::async_client          cli_;
        ConnectedLatch              latch_;
        CallbackRCU<ErrorCallback>* err_cb_ptr_{ nullptr };
    };

}  // namespace libmqtt