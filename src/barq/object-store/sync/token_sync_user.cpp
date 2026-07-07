////////////////////////////////////////////////////////////////////////////
//
// Copyright 2026 Barq
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

#include <barq/object-store/sync/token_sync_user.hpp>

#include <barq/object-store/sync/generic_network_transport.hpp>
#include <barq/sync/config.hpp>

#include <barq/error_codes.hpp>

#include <stdexcept>
#include <utility>

namespace barq {
namespace {

void validate_non_empty(const std::string& value, const char* name)
{
    if (value.empty()) {
        throw std::invalid_argument(std::string{name} + " must not be empty");
    }
}

void validate_partition(const std::string& partition)
{
    validate_non_empty(partition, "partition");
    if (partition.front() == '/') {
        throw std::invalid_argument("partition must be relative to the tenant");
    }
}

} // unnamed namespace

std::shared_ptr<TokenSyncUser> TokenSyncUser::create(std::string tenant_id, std::string user_id,
                                                     std::string access_token)
{
    return std::make_shared<TokenSyncUser>(std::move(tenant_id), std::move(user_id), std::move(access_token));
}

TokenSyncUser::TokenSyncUser(std::string tenant_id, std::string user_id, std::string access_token)
    : m_tenant_id(std::move(tenant_id))
    , m_user_id(std::move(user_id))
    , m_access_token(std::move(access_token))
    , m_manager(SyncManager::create(SyncClientConfig{}))
{
    validate_non_empty(m_tenant_id, "tenant_id");
    validate_non_empty(m_user_id, "user_id");
    validate_non_empty(m_access_token, "access_token");
}

// --- Barq-specific configuration -----------------------------------------

std::string TokenSyncUser::tenant_id() const
{
    return app_id();
}

std::string TokenSyncUser::route() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_route;
}

bool TokenSyncUser::has_route() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_route.empty();
}

void TokenSyncUser::set_route(std::string route, bool verified)
{
    validate_non_empty(route, "route");
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_route = route;
    }
    m_manager->set_sync_route(std::move(route), verified);
}

void TokenSyncUser::set_access_token(std::string access_token)
{
    validate_non_empty(access_token, "access_token");
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_access_token = access_token;
        m_access_token_refresh_required = false;
        m_state = State::LoggedIn;
    }
    m_manager->update_sessions_for(*this, State::LoggedIn, State::LoggedIn, access_token);
}

void TokenSyncUser::set_access_token_refresh_handler(AccessTokenRefreshHandler handler)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_refresh_handler = std::move(handler);
}

void TokenSyncUser::mark_access_token_refresh_required()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_access_token_refresh_required = true;
}

std::shared_ptr<SyncConfig> TokenSyncUser::make_sync_config(std::string partition)
{
    validate_partition(partition);
    if (!has_route()) {
        throw std::logic_error("sync route must be set before making a sync config");
    }
    return std::make_shared<SyncConfig>(shared_from_this(), std::move(partition));
}

std::shared_ptr<SyncConfig> TokenSyncUser::make_flexible_sync_config()
{
    if (!has_route()) {
        throw std::logic_error("sync route must be set before making a sync config");
    }
    return std::make_shared<SyncConfig>(shared_from_this(), SyncConfig::FLXSyncEnabled{});
}

// --- SyncUser interface --------------------------------------------------

std::string TokenSyncUser::user_id() const noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_user_id;
}

std::string TokenSyncUser::app_id() const noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_tenant_id;
}

std::string TokenSyncUser::access_token() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_access_token;
}

std::string TokenSyncUser::refresh_token() const
{
    return {};
}

SyncUser::State TokenSyncUser::state() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

bool TokenSyncUser::access_token_refresh_required() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_access_token_refresh_required;
}

SyncManager* TokenSyncUser::sync_manager()
{
    return m_manager.get();
}

void TokenSyncUser::request_log_out()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = State::LoggedOut;
    }
    m_manager->update_sessions_for(*this, State::LoggedIn, State::LoggedOut, {});
}

void TokenSyncUser::request_refresh_location(CompletionHandler&& completion)
{
    completion(std::nullopt);
}

void TokenSyncUser::request_access_token(CompletionHandler&& completion)
{
    AccessTokenRefreshHandler handler;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        handler = m_refresh_handler;
    }

    if (!handler) {
        completion(std::nullopt);
        return;
    }

    try {
        set_access_token(handler());
        completion(std::nullopt);
    }
    catch (const std::exception& e) {
        completion(networking::NetworkError(ErrorCodes::RuntimeError, e.what()));
    }
}

void TokenSyncUser::track_barq(std::string_view path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_tracked_barqs.emplace_back(path);
}

std::string TokenSyncUser::create_file_action(SyncFileAction, std::string_view, std::optional<std::string>)
{
    return {};
}

} // namespace barq
