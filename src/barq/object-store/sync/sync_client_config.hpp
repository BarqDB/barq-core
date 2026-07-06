////////////////////////////////////////////////////////////////////////////
//
// Copyright 2024 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#ifndef BARQ_OS_SYNC_CLIENT_CONFIG_HPP
#define BARQ_OS_SYNC_CLIENT_CONFIG_HPP

#include <barq/sync/binding_callback_thread_observer.hpp>
#include <barq/sync/config.hpp>
#include <barq/sync/socket_provider.hpp>
#include <barq/util/logger.hpp>

namespace barq {
struct SyncClientTimeouts {
    SyncClientTimeouts();
    // See sync::Client::Config for the meaning of these fields.
    uint64_t connect_timeout;
    uint64_t connection_linger_time;
    uint64_t ping_keepalive_period;
    uint64_t pong_keepalive_timeout;
    uint64_t fast_reconnect_limit;
    // Used for requesting location metadata at startup and reconnecting sync connections.
    // NOTE: delay_jitter_divisor is not configurable
    sync::ResumptionDelayInfo reconnect_backoff_info;
};

struct SyncClientConfig {
    using LoggerFactory = std::function<std::shared_ptr<util::Logger>(util::Logger::Level)>;
    LoggerFactory logger_factory;
    util::Logger::Level log_level = util::Logger::Level::info;
    ReconnectMode reconnect_mode = ReconnectMode::normal; // For internal sync-client testing only!
#if BARQ_DISABLE_SYNC_MULTIPLEXING
    bool multiplex_sessions = false;
#else
    bool multiplex_sessions = true;
#endif

    // The SyncSocket instance used by the Sync Client for event synchronization
    // and creating WebSockets. If not provided the default implementation will be used.
    std::shared_ptr<sync::SyncSocketProvider> socket_provider;

    // Optional thread observer for event loop thread events in the default SyncSocketProvider
    // implementation. It is not used for custom SyncSocketProvider implementations.
    std::shared_ptr<BindingCallbackThreadObserver> default_socket_provider_thread_observer;

    // {@
    // Optional information about the binding/application that is sent as part of the User-Agent
    // when establishing a connection to the server. These values are only used by the default
    // SyncSocket implementation. Custom SyncSocket implementations must update the User-Agent
    // directly, if supported by the platform APIs.
    std::string user_agent_binding_info;
    std::string user_agent_application_info;
    // @}

    SyncClientTimeouts timeouts;
};

} // namespace barq

#endif // BARQ_OS_SYNC_CLIENT_CONFIG_HPP
