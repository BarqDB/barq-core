////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2026 the Barq authors
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

#ifndef BARQ_OS_TOKEN_SYNC_USER_HPP
#define BARQ_OS_TOKEN_SYNC_USER_HPP

#include <barq/object-store/sync/sync_manager.hpp>
#include <barq/object-store/sync/sync_user.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace barq {

struct SyncConfig;

/// A concrete SyncUser identified by a tenant and a pre-supplied access token
/// (a signed JWT), rather than by an Atlas App Services login.
///
/// Barq authenticates every sync session with a token minted out-of-band, so
/// there is no `App` to produce a user. This is the one implementation of that
/// model: every Barq client (the C++ native SDK via this class directly, other
/// language SDKs via the C API wrappers in `c_api/sync.cpp`) shares it instead
/// of each re-implementing the twelve-method SyncUser interface.
class TokenSyncUser : public SyncUser, public std::enable_shared_from_this<TokenSyncUser> {
public:
    /// Called by the sync engine when the current access token has expired and a
    /// fresh one is needed. Returns the new token, or throws to signal failure.
    using AccessTokenRefreshHandler = std::function<std::string()>;

    /// Create a user for `tenant_id` (the app id) authenticating `user_id` with
    /// `access_token`. All three must be non-empty (throws std::invalid_argument
    /// otherwise). A route must be set before a sync config can be made.
    static std::shared_ptr<TokenSyncUser> create(std::string tenant_id, std::string user_id,
                                                 std::string access_token);

    // --- Barq-specific configuration -------------------------------------

    std::string tenant_id() const;
    std::string route() const;
    bool has_route() const;

    /// Set the websocket route to the sync server (e.g. ws://host:port/barq-sync).
    /// `verified` marks whether the route came from a trusted source.
    void set_route(std::string route, bool verified = true);

    void set_access_token(std::string access_token);
    void set_access_token_refresh_handler(AccessTokenRefreshHandler handler);
    void mark_access_token_refresh_required();

    /// Build a partition-based sync config for `partition` (must be non-empty and
    /// relative to the tenant, i.e. not starting with '/'). A route must be set.
    std::shared_ptr<SyncConfig> make_sync_config(std::string partition);

    /// Build a Flexible Sync config. A route must be set.
    std::shared_ptr<SyncConfig> make_flexible_sync_config();

    // --- SyncUser interface ----------------------------------------------

    std::string user_id() const noexcept override;
    std::string app_id() const noexcept override;
    std::string access_token() const override;
    std::string refresh_token() const override;
    State state() const override;
    bool access_token_refresh_required() const override;
    SyncManager* sync_manager() override;
    void request_log_out() override;
    void request_refresh_location(CompletionHandler&&) override;
    void request_access_token(CompletionHandler&&) override;
    void track_barq(std::string_view path) override;
    std::string create_file_action(SyncFileAction, std::string_view, std::optional<std::string>) override;

    // Use create(); constructed shared to satisfy enable_shared_from_this.
    TokenSyncUser(std::string tenant_id, std::string user_id, std::string access_token);

private:
    mutable std::mutex m_mutex;
    std::string m_tenant_id;
    std::string m_user_id;
    std::string m_access_token;
    std::string m_route;
    bool m_access_token_refresh_required = false;
    State m_state = State::LoggedIn;
    AccessTokenRefreshHandler m_refresh_handler;
    std::vector<std::string> m_tracked_barqs;
    std::shared_ptr<SyncManager> m_manager;
};

} // namespace barq

#endif // BARQ_OS_TOKEN_SYNC_USER_HPP
