////////////////////////////////////////////////////////////////////////////
//
// Copyright 2021 Realm Inc.
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

#include <barq/sync/config.hpp>
#include <barq/sync/client.hpp>
#include <barq/sync/protocol.hpp>
#include <barq/sync/network/websocket.hpp>
#include <barq/object-store/c_api/conversion.hpp>
#include <barq/object-store/sync/sync_manager.hpp>
#include <barq/object-store/sync/sync_session.hpp>
#include <barq/object-store/sync/async_open_task.hpp>
#include <barq/util/basic_system_errors.hpp>

#include "types.hpp"
#include "util.hpp"


barq_async_open_task_progress_notification_token::~barq_async_open_task_progress_notification_token()
{
    task->unregister_download_progress_notifier(token);
}

barq_sync_session_connection_state_notification_token::~barq_sync_session_connection_state_notification_token()
{
    session->unregister_connection_change_callback(token);
}

namespace barq::c_api {

static_assert(barq_sync_client_reconnect_mode_e(ReconnectMode::normal) == BARQ_SYNC_CLIENT_RECONNECT_MODE_NORMAL);
static_assert(barq_sync_client_reconnect_mode_e(ReconnectMode::testing) == BARQ_SYNC_CLIENT_RECONNECT_MODE_TESTING);

static_assert(barq_sync_session_resync_mode_e(ClientResyncMode::Manual) == BARQ_SYNC_SESSION_RESYNC_MODE_MANUAL);
static_assert(barq_sync_session_resync_mode_e(ClientResyncMode::DiscardLocal) ==
              BARQ_SYNC_SESSION_RESYNC_MODE_DISCARD_LOCAL);
static_assert(barq_sync_session_resync_mode_e(ClientResyncMode::Recover) == BARQ_SYNC_SESSION_RESYNC_MODE_RECOVER);
static_assert(barq_sync_session_resync_mode_e(ClientResyncMode::RecoverOrDiscard) ==
              BARQ_SYNC_SESSION_RESYNC_MODE_RECOVER_OR_DISCARD);

static_assert(barq_sync_session_stop_policy_e(SyncSessionStopPolicy::Immediately) ==
              BARQ_SYNC_SESSION_STOP_POLICY_IMMEDIATELY);
static_assert(barq_sync_session_stop_policy_e(SyncSessionStopPolicy::LiveIndefinitely) ==
              BARQ_SYNC_SESSION_STOP_POLICY_LIVE_INDEFINITELY);
static_assert(barq_sync_session_stop_policy_e(SyncSessionStopPolicy::AfterChangesUploaded) ==
              BARQ_SYNC_SESSION_STOP_POLICY_AFTER_CHANGES_UPLOADED);

static_assert(barq_sync_session_state_e(SyncSession::State::Active) == BARQ_SYNC_SESSION_STATE_ACTIVE);
static_assert(barq_sync_session_state_e(SyncSession::State::Dying) == BARQ_SYNC_SESSION_STATE_DYING);
static_assert(barq_sync_session_state_e(SyncSession::State::Inactive) == BARQ_SYNC_SESSION_STATE_INACTIVE);
static_assert(barq_sync_session_state_e(SyncSession::State::WaitingForAccessToken) ==
              BARQ_SYNC_SESSION_STATE_WAITING_FOR_ACCESS_TOKEN);
static_assert(barq_sync_session_state_e(SyncSession::State::Paused) == BARQ_SYNC_SESSION_STATE_PAUSED);

static_assert(barq_sync_connection_state_e(SyncSession::ConnectionState::Disconnected) ==
              BARQ_SYNC_CONNECTION_STATE_DISCONNECTED);
static_assert(barq_sync_connection_state_e(SyncSession::ConnectionState::Connecting) ==
              BARQ_SYNC_CONNECTION_STATE_CONNECTING);
static_assert(barq_sync_connection_state_e(SyncSession::ConnectionState::Connected) ==
              BARQ_SYNC_CONNECTION_STATE_CONNECTED);

static_assert(barq_sync_progress_direction_e(SyncSession::ProgressDirection::upload) ==
              BARQ_SYNC_PROGRESS_DIRECTION_UPLOAD);
static_assert(barq_sync_progress_direction_e(SyncSession::ProgressDirection::download) ==
              BARQ_SYNC_PROGRESS_DIRECTION_DOWNLOAD);


namespace {
using namespace barq::sync;
static_assert(barq_sync_error_action_e(ProtocolErrorInfo::Action::NoAction) == BARQ_SYNC_ERROR_ACTION_NO_ACTION);
static_assert(barq_sync_error_action_e(ProtocolErrorInfo::Action::ProtocolViolation) ==
              BARQ_SYNC_ERROR_ACTION_PROTOCOL_VIOLATION);
static_assert(barq_sync_error_action_e(ProtocolErrorInfo::Action::ApplicationBug) ==
              BARQ_SYNC_ERROR_ACTION_APPLICATION_BUG);
static_assert(barq_sync_error_action_e(ProtocolErrorInfo::Action::Warning) == BARQ_SYNC_ERROR_ACTION_WARNING);
static_assert(barq_sync_error_action_e(ProtocolErrorInfo::Action::Transient) == BARQ_SYNC_ERROR_ACTION_TRANSIENT);
static_assert(barq_sync_error_action_e(ProtocolErrorInfo::Action::DeleteBarq) ==
              BARQ_SYNC_ERROR_ACTION_DELETE_BARQ);
static_assert(barq_sync_error_action_e(ProtocolErrorInfo::Action::ClientReset) ==
              BARQ_SYNC_ERROR_ACTION_CLIENT_RESET);
static_assert(barq_sync_error_action_e(ProtocolErrorInfo::Action::ClientResetNoRecovery) ==
              BARQ_SYNC_ERROR_ACTION_CLIENT_RESET_NO_RECOVERY);
static_assert(barq_sync_error_action_e(ProtocolErrorInfo::Action::MigrateToFLX) ==
              BARQ_SYNC_ERROR_ACTION_MIGRATE_TO_FLX);
static_assert(barq_sync_error_action_e(ProtocolErrorInfo::Action::RevertToPBS) ==
              BARQ_SYNC_ERROR_ACTION_REVERT_TO_PBS);

static_assert(barq_flx_sync_subscription_set_state_e(SubscriptionSet::State::Pending) ==
              BARQ_SYNC_SUBSCRIPTION_PENDING);
static_assert(barq_flx_sync_subscription_set_state_e(SubscriptionSet::State::Bootstrapping) ==
              BARQ_SYNC_SUBSCRIPTION_BOOTSTRAPPING);
static_assert(barq_flx_sync_subscription_set_state_e(SubscriptionSet::State::AwaitingMark) ==
              BARQ_SYNC_SUBSCRIPTION_AWAITING_MARK);
static_assert(barq_flx_sync_subscription_set_state_e(SubscriptionSet::State::Complete) ==
              BARQ_SYNC_SUBSCRIPTION_COMPLETE);
static_assert(barq_flx_sync_subscription_set_state_e(SubscriptionSet::State::Error) == BARQ_SYNC_SUBSCRIPTION_ERROR);
static_assert(barq_flx_sync_subscription_set_state_e(SubscriptionSet::State::Superseded) ==
              BARQ_SYNC_SUBSCRIPTION_SUPERSEDED);
static_assert(barq_flx_sync_subscription_set_state_e(SubscriptionSet::State::Uncommitted) ==
              BARQ_SYNC_SUBSCRIPTION_UNCOMMITTED);

static_assert(barq_sync_file_action(SyncFileAction::DeleteBarq) == BARQ_SYNC_FILE_ACTION_DELETE_BARQ);
static_assert(barq_sync_file_action(SyncFileAction::BackUpThenDeleteBarq) ==
              BARQ_SYNC_FILE_ACTION_BACK_UP_THEN_DELETE_BARQ);

} // namespace


static Query add_ordering_to_barq_query(Query barq_query, const DescriptorOrdering& ordering)
{
    auto ordering_copy = util::make_bind<DescriptorOrdering>();
    *ordering_copy = ordering;
    barq_query.set_ordering(ordering_copy);
    return barq_query;
}

BARQ_API barq_sync_client_config_t* barq_sync_client_config_new(void) noexcept
{
    return new barq_sync_client_config_t;
}

BARQ_API void barq_sync_client_config_set_reconnect_mode(barq_sync_client_config_t* config,
                                                         barq_sync_client_reconnect_mode_e mode) noexcept
{
    config->reconnect_mode = ReconnectMode(mode);
}
BARQ_API void barq_sync_client_config_set_multiplex_sessions(barq_sync_client_config_t* config,
                                                             bool multiplex) noexcept
{
    config->multiplex_sessions = multiplex;
}

BARQ_API void barq_sync_client_config_set_user_agent_binding_info(barq_sync_client_config_t* config,
                                                                  const char* info) noexcept
{
    config->user_agent_binding_info = info;
}

BARQ_API void barq_sync_client_config_set_user_agent_application_info(barq_sync_client_config_t* config,
                                                                      const char* info) noexcept
{
    config->user_agent_application_info = info;
}

BARQ_API void barq_sync_client_config_set_connect_timeout(barq_sync_client_config_t* config,
                                                          uint64_t timeout) noexcept
{
    config->timeouts.connect_timeout = timeout;
}

BARQ_API void barq_sync_client_config_set_connection_linger_time(barq_sync_client_config_t* config,
                                                                 uint64_t time) noexcept
{
    config->timeouts.connection_linger_time = time;
}

BARQ_API void barq_sync_client_config_set_ping_keepalive_period(barq_sync_client_config_t* config,
                                                                uint64_t period) noexcept
{
    config->timeouts.ping_keepalive_period = period;
}

BARQ_API void barq_sync_client_config_set_pong_keepalive_timeout(barq_sync_client_config_t* config,
                                                                 uint64_t timeout) noexcept
{
    config->timeouts.pong_keepalive_timeout = timeout;
}

BARQ_API void barq_sync_client_config_set_fast_reconnect_limit(barq_sync_client_config_t* config,
                                                               uint64_t limit) noexcept
{
    config->timeouts.fast_reconnect_limit = limit;
}

BARQ_API void barq_sync_client_config_set_resumption_delay_interval(barq_sync_client_config_t* config,
                                                                    uint64_t interval) noexcept
{
    config->timeouts.reconnect_backoff_info.resumption_delay_interval = std::chrono::milliseconds{interval};
}

BARQ_API void barq_sync_client_config_set_max_resumption_delay_interval(barq_sync_client_config_t* config,
                                                                        uint64_t interval) noexcept
{
    config->timeouts.reconnect_backoff_info.max_resumption_delay_interval = std::chrono::milliseconds{interval};
}

BARQ_API void barq_sync_client_config_set_resumption_delay_backoff_multiplier(barq_sync_client_config_t* config,
                                                                              int multiplier) noexcept
{
    config->timeouts.reconnect_backoff_info.resumption_delay_backoff_multiplier = multiplier;
}

/// Register an app local callback handler for bindings interested in registering callbacks before/after
/// the ObjectStore thread runs for this app. This only works for the default socket provider implementation.
/// IMPORTANT: If a function is supplied that handles the exception, it must call abort() or cause the
/// application to crash since the SyncClient will be in a bad state if this occurs and will not be able to
/// shut down properly.
/// @param config pointer to sync client config created by barq_sync_client_config_new()
/// @param on_thread_create callback invoked when the object store thread is created
/// @param on_thread_destroy callback invoked when the object store thread is destroyed
/// @param on_error callback invoked to signal to the listener that some error has occurred.
/// @param user_data pointer to user defined data that is provided to each of the callback functions
/// @param free_userdata callback invoked when the user_data is to be freed
BARQ_API void barq_sync_client_config_set_default_binding_thread_observer(
    barq_sync_client_config_t* config, barq_on_object_store_thread_callback_t on_thread_create,
    barq_on_object_store_thread_callback_t on_thread_destroy, barq_on_object_store_error_callback_t on_error,
    barq_userdata_t user_data, barq_free_userdata_func_t free_userdata)
{
    config->default_socket_provider_thread_observer = std::make_shared<CBindingThreadObserver>(
        on_thread_create, on_thread_destroy, on_error, user_data, free_userdata);
}

BARQ_API void barq_config_set_sync_config(barq_config_t* config, barq_sync_config_t* sync_config)
{
    config->sync_config = std::make_shared<SyncConfig>(*sync_config);
}

BARQ_API barq_sync_config_t* barq_sync_config_new(const barq_user_t* user, const char* partition_value) noexcept
{
    return new barq_sync_config_t(*user, partition_value);
}

BARQ_API barq_sync_config_t* barq_flx_sync_config_new(const barq_user_t* user) noexcept
{
    return new barq_sync_config(*user, barq::SyncConfig::FLXSyncEnabled{});
}

BARQ_API void barq_sync_config_set_session_stop_policy(barq_sync_config_t* config,
                                                       barq_sync_session_stop_policy_e policy) noexcept
{
    config->stop_policy = SyncSessionStopPolicy(policy);
}

BARQ_API void barq_sync_config_set_error_handler(barq_sync_config_t* config, barq_sync_error_handler_func_t handler,
                                                 barq_userdata_t userdata,
                                                 barq_free_userdata_func_t userdata_free) noexcept
{
    auto cb = [handler, userdata = SharedUserdata(userdata, FreeUserdata(userdata_free))](
                  std::shared_ptr<SyncSession> session, SyncError error) {
        auto c_error = barq_sync_error_t();

        std::string error_code_message;
        c_error.status = to_capi(error.status);
        c_error.is_fatal = error.is_fatal;
        c_error.is_unrecognized_by_client = error.is_unrecognized_by_client;
        c_error.is_client_reset_requested = error.is_client_reset_requested();
        c_error.server_requests_action = static_cast<barq_sync_error_action_e>(error.server_requests_action);
        c_error.c_original_file_path_key = error.c_original_file_path_key;
        c_error.c_recovery_file_path_key = error.c_recovery_file_path_key;
        c_error.user_code_error = ErrorStorage::get_thread_local()->get_and_clear_user_code_error();

        std::vector<barq_sync_error_user_info_t> c_user_info;
        c_user_info.reserve(error.user_info.size());
        for (auto& info : error.user_info) {
            c_user_info.push_back({info.first.c_str(), info.second.c_str()});
        }

        c_error.user_info_map = c_user_info.data();
        c_error.user_info_length = c_user_info.size();

        std::vector<barq_sync_error_compensating_write_info_t> c_compensating_writes;
        for (const auto& compensating_write : error.compensating_writes_info) {
            c_compensating_writes.push_back({compensating_write.reason.c_str(),
                                             compensating_write.object_name.c_str(),
                                             to_capi(compensating_write.primary_key)});
        }
        c_error.compensating_writes = c_compensating_writes.data();
        c_error.compensating_writes_length = c_compensating_writes.size();

        barq_sync_session_t c_session(session);
        handler(userdata.get(), &c_session, std::move(c_error));
    };
    config->error_handler = std::move(cb);
}

BARQ_API void barq_sync_config_set_client_validate_ssl(barq_sync_config_t* config, bool validate) noexcept
{
    config->client_validate_ssl = validate;
}

BARQ_API void barq_sync_config_set_ssl_trust_certificate_path(barq_sync_config_t* config, const char* path) noexcept
{
    config->ssl_trust_certificate_path = std::string(path);
}

BARQ_API void barq_sync_config_set_ssl_verify_callback(barq_sync_config_t* config,
                                                       barq_sync_ssl_verify_func_t callback,
                                                       barq_userdata_t userdata,
                                                       barq_free_userdata_func_t userdata_free) noexcept
{
    auto cb = [callback, userdata = SharedUserdata(userdata, FreeUserdata(userdata_free))](
                  const std::string& server_address, SyncConfig::ProxyConfig::port_type server_port,
                  const char* pem_data, size_t pem_size, int preverify_ok, int depth) {
        return callback(userdata.get(), server_address.c_str(), server_port, pem_data, pem_size, preverify_ok, depth);
    };

    config->ssl_verify_callback = std::move(cb);
}

BARQ_API void barq_sync_config_set_cancel_waits_on_nonfatal_error(barq_sync_config_t* config, bool cancel) noexcept
{
    config->cancel_waits_on_nonfatal_error = cancel;
}

BARQ_API void barq_sync_config_set_authorization_header_name(barq_sync_config_t* config, const char* name) noexcept
{
    config->authorization_header_name = std::string(name);
}

BARQ_API void barq_sync_config_set_custom_http_header(barq_sync_config_t* config, const char* name,
                                                      const char* value) noexcept
{
    config->custom_http_headers[name] = value;
}

BARQ_API void barq_sync_config_set_recovery_directory_path(barq_sync_config_t* config, const char* path) noexcept
{
    config->recovery_directory = std::string(path);
}

BARQ_API void barq_sync_config_set_resync_mode(barq_sync_config_t* config,
                                               barq_sync_session_resync_mode_e mode) noexcept
{
    config->client_resync_mode = ClientResyncMode(mode);
}

BARQ_API barq_object_id_t barq_sync_subscription_id(const barq_flx_sync_subscription_t* subscription) noexcept
{
    BARQ_ASSERT(subscription != nullptr);
    return to_capi(subscription->id);
}

BARQ_API barq_string_t barq_sync_subscription_name(const barq_flx_sync_subscription_t* subscription) noexcept
{
    BARQ_ASSERT(subscription != nullptr);
    return to_capi(subscription->name);
}

BARQ_API barq_string_t
barq_sync_subscription_object_class_name(const barq_flx_sync_subscription_t* subscription) noexcept
{
    BARQ_ASSERT(subscription != nullptr);
    return to_capi(subscription->object_class_name);
}

BARQ_API barq_string_t
barq_sync_subscription_query_string(const barq_flx_sync_subscription_t* subscription) noexcept
{
    BARQ_ASSERT(subscription != nullptr);
    return to_capi(subscription->query_string);
}

BARQ_API barq_timestamp_t
barq_sync_subscription_created_at(const barq_flx_sync_subscription_t* subscription) noexcept
{
    BARQ_ASSERT(subscription != nullptr);
    return to_capi(subscription->created_at);
}

BARQ_API barq_timestamp_t
barq_sync_subscription_updated_at(const barq_flx_sync_subscription_t* subscription) noexcept
{
    BARQ_ASSERT(subscription != nullptr);
    return to_capi(subscription->updated_at);
}

BARQ_API void barq_sync_config_set_before_client_reset_handler(barq_sync_config_t* config,
                                                               barq_sync_before_client_reset_func_t callback,
                                                               barq_userdata_t userdata,
                                                               barq_free_userdata_func_t userdata_free) noexcept
{
    auto cb = [callback, userdata = SharedUserdata(userdata, FreeUserdata(userdata_free))](SharedBarq before_barq) {
        barq_t r1{before_barq};
        if (!callback(userdata.get(), &r1)) {
            throw CallbackFailed{};
        }
    };
    config->notify_before_client_reset = std::move(cb);
}

BARQ_API void barq_sync_config_set_after_client_reset_handler(barq_sync_config_t* config,
                                                              barq_sync_after_client_reset_func_t callback,
                                                              barq_userdata_t userdata,
                                                              barq_free_userdata_func_t userdata_free) noexcept
{
    auto cb = [callback, userdata = SharedUserdata(userdata, FreeUserdata(userdata_free))](
                  SharedBarq before_barq, ThreadSafeReference after_barq, bool did_recover) {
        barq_t r1{before_barq};
        auto tsr = barq_t::thread_safe_reference(std::move(after_barq));
        if (!callback(userdata.get(), &r1, &tsr, did_recover)) {
            throw CallbackFailed{};
        }
    };
    config->notify_after_client_reset = std::move(cb);
}

BARQ_API void barq_sync_config_set_initial_subscription_handler(
    barq_sync_config_t* config, barq_async_open_task_init_subscription_func_t callback, bool rerun_on_open,
    barq_userdata_t userdata, barq_free_userdata_func_t userdata_free)
{
    auto cb = [callback,
               userdata = SharedUserdata(userdata, FreeUserdata(userdata_free))](ThreadSafeReference barq) {
        auto tsr = new barq_t::thread_safe_reference(std::move(barq));
        callback(tsr, userdata.get());
    };
    config->subscription_initializer = std::move(cb);
    config->rerun_init_subscription_on_open = rerun_on_open;
}

BARQ_API barq_flx_sync_subscription_set_t* barq_sync_get_latest_subscription_set(const barq_t* barq)
{
    BARQ_ASSERT(barq != nullptr);
    return wrap_err([&]() {
        return new barq_flx_sync_subscription_set_t((*barq)->get_latest_subscription_set());
    });
}

BARQ_API barq_flx_sync_subscription_set_t* barq_sync_get_active_subscription_set(const barq_t* barq)
{
    BARQ_ASSERT(barq != nullptr);
    return wrap_err([&]() {
        return new barq_flx_sync_subscription_set_t((*barq)->get_active_subscription_set());
    });
}

BARQ_API barq_flx_sync_subscription_set_state_e
barq_sync_on_subscription_set_state_change_wait(const barq_flx_sync_subscription_set_t* subscription_set,
                                                 barq_flx_sync_subscription_set_state_e notify_when) noexcept
{
    BARQ_ASSERT(subscription_set != nullptr);
    SubscriptionSet::State state =
        subscription_set->get_state_change_notification(static_cast<SubscriptionSet::State>(notify_when)).get();
    return static_cast<barq_flx_sync_subscription_set_state_e>(state);
}

BARQ_API bool
barq_sync_on_subscription_set_state_change_async(const barq_flx_sync_subscription_set_t* subscription_set,
                                                  barq_flx_sync_subscription_set_state_e notify_when,
                                                  barq_sync_on_subscription_state_changed_t callback,
                                                  barq_userdata_t userdata, barq_free_userdata_func_t userdata_free)
{
    BARQ_ASSERT(subscription_set != nullptr && callback != nullptr);
    return wrap_err([&]() {
        auto future_state =
            subscription_set->get_state_change_notification(static_cast<SubscriptionSet::State>(notify_when));
        std::move(future_state)
            .get_async([callback, userdata = SharedUserdata(userdata, FreeUserdata(userdata_free))](
                           const StatusWith<SubscriptionSet::State>& state) -> void {
                if (state.is_ok())
                    callback(userdata.get(), static_cast<barq_flx_sync_subscription_set_state_e>(state.get_value()));
                else
                    callback(userdata.get(), barq_flx_sync_subscription_set_state_e::BARQ_SYNC_SUBSCRIPTION_ERROR);
            });
        return true;
    });
}

BARQ_API int64_t
barq_sync_subscription_set_version(const barq_flx_sync_subscription_set_t* subscription_set) noexcept
{
    BARQ_ASSERT(subscription_set != nullptr);
    return subscription_set->version();
}

BARQ_API barq_flx_sync_subscription_set_state_e
barq_sync_subscription_set_state(const barq_flx_sync_subscription_set_t* subscription_set) noexcept
{
    BARQ_ASSERT(subscription_set != nullptr);
    return static_cast<barq_flx_sync_subscription_set_state_e>(subscription_set->state());
}

BARQ_API const char*
barq_sync_subscription_set_error_str(const barq_flx_sync_subscription_set_t* subscription_set) noexcept
{
    BARQ_ASSERT(subscription_set != nullptr);
    return subscription_set->error_str().data();
}

BARQ_API size_t barq_sync_subscription_set_size(const barq_flx_sync_subscription_set_t* subscription_set) noexcept
{
    BARQ_ASSERT(subscription_set != nullptr);
    return subscription_set->size();
}

BARQ_API barq_flx_sync_subscription_t*
barq_sync_find_subscription_by_name(const barq_flx_sync_subscription_set_t* subscription_set,
                                     const char* name) noexcept
{
    BARQ_ASSERT(subscription_set != nullptr);
    auto ptr = subscription_set->find(name);
    if (!ptr)
        return nullptr;
    return new barq_flx_sync_subscription_t(*ptr);
}

BARQ_API barq_flx_sync_subscription_t*
barq_sync_find_subscription_by_results(const barq_flx_sync_subscription_set_t* subscription_set,
                                        barq_results_t* results) noexcept
{
    BARQ_ASSERT(subscription_set != nullptr);
    auto barq_query = add_ordering_to_barq_query(results->get_query(), results->get_ordering());
    auto ptr = subscription_set->find(barq_query);
    if (!ptr)
        return nullptr;
    return new barq_flx_sync_subscription_t{*ptr};
}

BARQ_API barq_flx_sync_subscription_t*
barq_sync_subscription_at(const barq_flx_sync_subscription_set_t* subscription_set, size_t index)
{
    BARQ_ASSERT(subscription_set != nullptr && index < subscription_set->size());
    try {
        return new barq_flx_sync_subscription_t{subscription_set->at(index)};
    }
    catch (...) {
        return nullptr;
    }
}

BARQ_API barq_flx_sync_subscription_t*
barq_sync_find_subscription_by_query(const barq_flx_sync_subscription_set_t* subscription_set,
                                      barq_query_t* query) noexcept
{
    BARQ_ASSERT(subscription_set != nullptr);
    auto barq_query = add_ordering_to_barq_query(query->get_query(), query->get_ordering());
    auto ptr = subscription_set->find(barq_query);
    if (!ptr)
        return nullptr;
    return new barq_flx_sync_subscription_t(*ptr);
}

BARQ_API bool barq_sync_subscription_set_refresh(barq_flx_sync_subscription_set_t* subscription_set)
{
    BARQ_ASSERT(subscription_set != nullptr);
    return wrap_err([&]() {
        subscription_set->refresh();
        return true;
    });
}

BARQ_API barq_flx_sync_mutable_subscription_set_t*
barq_sync_make_subscription_set_mutable(barq_flx_sync_subscription_set_t* subscription_set)
{
    BARQ_ASSERT(subscription_set != nullptr);
    return wrap_err([&]() {
        return new barq_flx_sync_mutable_subscription_set_t{subscription_set->make_mutable_copy()};
    });
}

BARQ_API bool barq_sync_subscription_set_clear(barq_flx_sync_mutable_subscription_set_t* subscription_set)
{
    BARQ_ASSERT(subscription_set != nullptr);
    return wrap_err([&]() {
        subscription_set->clear();
        return true;
    });
}

BARQ_API bool
barq_sync_subscription_set_insert_or_assign_results(barq_flx_sync_mutable_subscription_set_t* subscription_set,
                                                     barq_results_t* results, const char* name, size_t* index,
                                                     bool* inserted)
{
    BARQ_ASSERT(subscription_set != nullptr && results != nullptr);
    return wrap_err([&]() {
        auto barq_query = add_ordering_to_barq_query(results->get_query(), results->get_ordering());
        const auto [it, successful] = name ? subscription_set->insert_or_assign(name, barq_query)
                                           : subscription_set->insert_or_assign(barq_query);
        *index = std::distance(subscription_set->begin(), it);
        *inserted = successful;
        return true;
    });
}

BARQ_API bool
barq_sync_subscription_set_insert_or_assign_query(barq_flx_sync_mutable_subscription_set_t* subscription_set,
                                                   barq_query_t* query, const char* name, size_t* index,
                                                   bool* inserted)
{
    BARQ_ASSERT(subscription_set != nullptr && query != nullptr);
    return wrap_err([&]() {
        auto barq_query = add_ordering_to_barq_query(query->get_query(), query->get_ordering());
        const auto [it, successful] = name ? subscription_set->insert_or_assign(name, barq_query)
                                           : subscription_set->insert_or_assign(barq_query);
        *index = std::distance(subscription_set->begin(), it);
        *inserted = successful;
        return true;
    });
}

BARQ_API bool barq_sync_subscription_set_erase_by_id(barq_flx_sync_mutable_subscription_set_t* subscription_set,
                                                     const barq_object_id_t* id, bool* erased)
{
    BARQ_ASSERT(subscription_set != nullptr && id != nullptr);
    *erased = false;
    return wrap_err([&] {
        *erased = subscription_set->erase_by_id(from_capi(*id));
        return true;
    });
}

BARQ_API bool barq_sync_subscription_set_erase_by_name(barq_flx_sync_mutable_subscription_set_t* subscription_set,
                                                       const char* name, bool* erased)
{
    BARQ_ASSERT(subscription_set != nullptr && name != nullptr);
    *erased = false;
    return wrap_err([&]() {
        *erased = subscription_set->erase(name);
        return true;
    });
}

BARQ_API bool barq_sync_subscription_set_erase_by_query(barq_flx_sync_mutable_subscription_set_t* subscription_set,
                                                        barq_query_t* query, bool* erased)
{
    BARQ_ASSERT(subscription_set != nullptr && query != nullptr);
    *erased = false;
    return wrap_err([&]() {
        auto barq_query = add_ordering_to_barq_query(query->get_query(), query->get_ordering());
        *erased = subscription_set->erase(barq_query);
        return true;
    });
}

BARQ_API bool barq_sync_subscription_set_erase_by_results(barq_flx_sync_mutable_subscription_set_t* subscription_set,
                                                          barq_results_t* results, bool* erased)
{
    BARQ_ASSERT(subscription_set != nullptr && results != nullptr);
    *erased = false;
    return wrap_err([&]() {
        auto barq_query = add_ordering_to_barq_query(results->get_query(), results->get_ordering());
        *erased = subscription_set->erase(barq_query);
        return true;
    });
}

BARQ_API bool
barq_sync_subscription_set_erase_by_class_name(barq_flx_sync_mutable_subscription_set_t* subscription_set,
                                                const char* object_class_name, bool* erased)
{
    BARQ_ASSERT(subscription_set != nullptr && object_class_name != nullptr);
    *erased = false;
    return wrap_err([&]() {
        *erased = subscription_set->erase_by_class_name(object_class_name);
        return true;
    });
}

BARQ_API barq_flx_sync_subscription_set_t*
barq_sync_subscription_set_commit(barq_flx_sync_mutable_subscription_set_t* subscription_set)
{
    BARQ_ASSERT(subscription_set != nullptr);
    return wrap_err([&]() {
        return new barq_flx_sync_subscription_set_t{std::move(*subscription_set).commit()};
    });
}

BARQ_API barq_async_open_task_t* barq_open_synchronized(barq_config_t* config) noexcept
{
    return wrap_err([config] {
        return new barq_async_open_task_t(Barq::get_synchronized_barq(*config));
    });
}

BARQ_API void barq_async_open_task_start(barq_async_open_task_t* task, barq_async_open_task_completion_func_t done,
                                         barq_userdata_t userdata, barq_free_userdata_func_t userdata_free) noexcept
{
    auto cb = [done, userdata = SharedUserdata(userdata, FreeUserdata(userdata_free))](ThreadSafeReference barq,
                                                                                       std::exception_ptr error) {
        if (error) {
            barq_async_error_t c_error(std::move(error));
            done(userdata.get(), nullptr, &c_error);
        }
        else {
            auto tsr = new barq_t::thread_safe_reference(std::move(barq));
            done(userdata.get(), tsr, nullptr);
        }
    };
    (*task)->start(std::move(cb));
}

BARQ_API void barq_async_open_task_cancel(barq_async_open_task_t* task) noexcept
{
    (*task)->cancel();
}

BARQ_API barq_async_open_task_progress_notification_token_t*
barq_async_open_task_register_download_progress_notifier(barq_async_open_task_t* task,
                                                          barq_sync_progress_func_t notifier,
                                                          barq_userdata_t userdata,
                                                          barq_free_userdata_func_t userdata_free) noexcept
{
    auto cb = [notifier, userdata = SharedUserdata(userdata, FreeUserdata(userdata_free))](
                  uint64_t transferred, uint64_t transferrable, double progress_estimate) {
        notifier(userdata.get(), transferred, transferrable, progress_estimate);
    };
    auto token = (*task)->register_download_progress_notifier(std::move(cb));
    return new barq_async_open_task_progress_notification_token_t{(*task), token};
}

BARQ_API barq_sync_session_t* barq_sync_session_get(const barq_t* barq) noexcept
{
    if (auto session = (*barq)->sync_session()) {
        return new barq_sync_session_t(std::move(session));
    }

    return nullptr;
}

BARQ_API barq_sync_session_state_e barq_sync_session_get_state(const barq_sync_session_t* session) noexcept
{
    return barq_sync_session_state_e((*session)->state());
}

BARQ_API barq_sync_connection_state_e
barq_sync_session_get_connection_state(const barq_sync_session_t* session) noexcept
{
    return barq_sync_connection_state_e((*session)->connection_state());
}

BARQ_API barq_user_t* barq_sync_session_get_user(const barq_sync_session_t* session) noexcept
{
    return new barq_user_t((*session)->user());
}

BARQ_API const char* barq_sync_session_get_partition_value(const barq_sync_session_t* session) noexcept
{
    return (*session)->config().partition_value.c_str();
}

BARQ_API const char* barq_sync_session_get_file_path(const barq_sync_session_t* session) noexcept
{
    return (*session)->path().c_str();
}

BARQ_API void barq_sync_session_pause(barq_sync_session_t* session) noexcept
{
    (*session)->pause();
}

BARQ_API void barq_sync_session_resume(barq_sync_session_t* session) noexcept
{
    (*session)->resume();
}

BARQ_API void barq_sync_session_get_file_ident(barq_sync_session_t* session, barq_salted_file_ident_t* out) noexcept
{
    auto file_ident = (*session)->get_file_ident();
    out->ident = file_ident.ident;
    out->salt = file_ident.salt;
}

BARQ_API barq_sync_session_connection_state_notification_token_t*
barq_sync_session_register_connection_state_change_callback(barq_sync_session_t* session,
                                                             barq_sync_connection_state_changed_func_t callback,
                                                             barq_userdata_t userdata,
                                                             barq_free_userdata_func_t userdata_free) noexcept
{
    std::function<barq::SyncSession::ConnectionStateChangeCallback> cb =
        [callback, userdata = SharedUserdata(userdata, FreeUserdata(userdata_free))](auto old_state, auto new_state) {
            callback(userdata.get(), barq_sync_connection_state_e(old_state),
                     barq_sync_connection_state_e(new_state));
        };
    auto token = (*session)->register_connection_change_callback(std::move(cb));
    return new barq_sync_session_connection_state_notification_token_t{(*session), token};
}

BARQ_API barq_sync_session_connection_state_notification_token_t* barq_sync_session_register_progress_notifier(
    barq_sync_session_t* session, barq_sync_progress_func_t notifier, barq_sync_progress_direction_e direction,
    bool is_streaming, barq_userdata_t userdata, barq_free_userdata_func_t userdata_free) noexcept
{
    std::function<barq::SyncSession::ProgressNotifierCallback> cb =
        [notifier, userdata = SharedUserdata(userdata, FreeUserdata(userdata_free))](
            uint64_t transferred, uint64_t transferrable, double progress_estimate) {
            notifier(userdata.get(), transferred, transferrable, progress_estimate);
        };
    auto token = (*session)->register_progress_notifier(std::move(cb), SyncSession::ProgressDirection(direction),
                                                        is_streaming);
    return new barq_sync_session_connection_state_notification_token_t{(*session), token};
}

BARQ_API void barq_sync_session_wait_for_download_completion(barq_sync_session_t* session,
                                                             barq_sync_wait_for_completion_func_t done,
                                                             barq_userdata_t userdata,
                                                             barq_free_userdata_func_t userdata_free) noexcept
{
    util::UniqueFunction<void(Status)> cb =
        [done, userdata = SharedUserdata(userdata, FreeUserdata(userdata_free))](Status s) {
            if (!s.is_ok()) {
                barq_error_t error = to_capi(s);
                done(userdata.get(), &error);
            }
            else {
                done(userdata.get(), nullptr);
            }
        };
    (*session)->wait_for_download_completion(std::move(cb));
}

BARQ_API void barq_sync_session_wait_for_upload_completion(barq_sync_session_t* session,
                                                           barq_sync_wait_for_completion_func_t done,
                                                           barq_userdata_t userdata,
                                                           barq_free_userdata_func_t userdata_free) noexcept
{
    util::UniqueFunction<void(Status)> cb =
        [done, userdata = SharedUserdata(userdata, FreeUserdata(userdata_free))](Status s) {
            if (!s.is_ok()) {
                barq_error_t error = to_capi(s);
                done(userdata.get(), &error);
            }
            else {
                done(userdata.get(), nullptr);
            }
        };
    (*session)->wait_for_upload_completion(std::move(cb));
}

BARQ_API void barq_sync_session_handle_error_for_testing(const barq_sync_session_t* session,
                                                         barq_errno_e error_code, const char* error_str,
                                                         bool is_fatal)
{
    BARQ_ASSERT(session);
    SyncSession::OnlyForTesting::handle_error(
        *session->get(),
        sync::SessionErrorInfo{Status{static_cast<ErrorCodes::Error>(error_code), error_str}, is_fatal});
}

} // namespace barq::c_api
