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

#include "types.hpp"
#include "util.hpp"
#include "conversion.hpp"

#include <barq/object-store/sync/sync_user.hpp>

namespace barq::c_api {
using namespace barq::app;

static_assert(barq_user_state_e(SyncUser::State::LoggedOut) == BARQ_USER_STATE_LOGGED_OUT);
static_assert(barq_user_state_e(SyncUser::State::LoggedIn) == BARQ_USER_STATE_LOGGED_IN);
static_assert(barq_user_state_e(SyncUser::State::Removed) == BARQ_USER_STATE_REMOVED);


static void cb_proxy_for_completion(barq_userdata_t userdata, const barq_app_error_t* err)
{
    SyncUser::CompletionHandler* cxx_cb = static_cast<SyncUser::CompletionHandler*>(userdata);
    BARQ_ASSERT(cxx_cb);
    std::optional<AppError> cxx_err;
    if (err) {
        std::optional<int> additional_error_code;
        if (err->http_status_code) {
            additional_error_code = err->http_status_code;
        }
        cxx_err =
            AppError(ErrorCodes::Error(err->error), err->message, err->link_to_server_logs, additional_error_code);
    }
    (*cxx_cb)(cxx_err);
    delete cxx_cb;
}

struct CAPIAppUser : SyncUser {
    void* m_userdata = nullptr;
    barq_free_userdata_func_t m_free = nullptr;
    const std::string m_app_id;
    const std::string m_user_id;
    barq_user_get_access_token_cb_t m_access_token_cb = nullptr;
    barq_user_get_refresh_token_cb_t m_refresh_token_cb = nullptr;
    barq_user_state_cb_t m_state_cb = nullptr;
    barq_user_access_token_refresh_required_cb_t m_atrr_cb = nullptr;
    barq_user_get_sync_manager_cb_t m_sync_manager_cb = nullptr;
    barq_user_request_log_out_cb_t m_request_log_out_cb = nullptr;
    barq_user_request_refresh_location_cb_t m_request_refresh_location_cb = nullptr;
    barq_user_request_access_token_cb_t m_request_access_token_cb = nullptr;
    barq_user_track_barq_cb_t m_track_barq_cb = nullptr;
    barq_user_create_file_action_cb_t m_create_fa_cb = nullptr;

    CAPIAppUser(const char* app_id, const char* user_id)
        : m_app_id(app_id)
        , m_user_id(user_id)
    {
    }
    CAPIAppUser(CAPIAppUser&& other)
        : m_userdata(std::exchange(other.m_userdata, nullptr))
        , m_free(std::exchange(other.m_free, nullptr))
        , m_app_id(std::move(other.m_app_id))
        , m_user_id(std::move(other.m_user_id))
        , m_access_token_cb(std::move(other.m_access_token_cb))
        , m_refresh_token_cb(std::move(other.m_refresh_token_cb))
        , m_state_cb(std::move(other.m_state_cb))
        , m_atrr_cb(std::move(other.m_atrr_cb))
        , m_sync_manager_cb(std::move(other.m_sync_manager_cb))
        , m_request_log_out_cb(std::move(other.m_request_log_out_cb))
        , m_request_refresh_location_cb(std::move(other.m_request_refresh_location_cb))
        , m_request_access_token_cb(std::move(other.m_request_access_token_cb))
        , m_track_barq_cb(std::move(other.m_track_barq_cb))
        , m_create_fa_cb(std::move(other.m_create_fa_cb))
    {
    }

    ~CAPIAppUser()
    {
        if (m_free)
            m_free(m_userdata);
    }
    std::string user_id() const noexcept override
    {
        return m_user_id;
    }
    std::string app_id() const noexcept override
    {
        return m_app_id;
    }
    std::string access_token() const override
    {
        return m_access_token_cb(m_userdata);
    }
    std::string refresh_token() const override
    {
        return m_refresh_token_cb(m_userdata);
    }
    State state() const override
    {
        return State(m_state_cb(m_userdata));
    }
    bool access_token_refresh_required() const override
    {
        return m_atrr_cb(m_userdata);
    }
    SyncManager* sync_manager() override
    {
        auto value = m_sync_manager_cb(m_userdata);
        if (value && value->get()) {
            return (value->get());
        }
        return nullptr;
    }
    void request_log_out() override
    {
        m_request_log_out_cb(m_userdata);
    }
    void request_refresh_location(CompletionHandler&& callback) override
    {
        auto unscoped_cb = new CompletionHandler(std::move(callback));
        m_request_refresh_location_cb(m_userdata, cb_proxy_for_completion, unscoped_cb);
    }
    void request_access_token(CompletionHandler&& callback) override
    {
        auto unscoped_cb = new CompletionHandler(std::move(callback));
        m_request_access_token_cb(m_userdata, cb_proxy_for_completion, unscoped_cb);
    }
    void track_barq(std::string_view path) override
    {
        if (m_track_barq_cb) {
            m_track_barq_cb(m_userdata, path.data());
        }
    }
    std::string create_file_action(SyncFileAction a, std::string_view path,
                                   std::optional<std::string> recovery_dir) override
    {

        if (m_create_fa_cb) {
            return m_create_fa_cb(m_userdata, barq_sync_file_action_e(a), path.data(),
                                  recovery_dir ? recovery_dir->data() : nullptr);
        }
        return "";
    }
};

BARQ_API barq_user_t* barq_user_new(barq_sync_user_create_config_t c) noexcept
{
    // optional to provide:
    // m_userdata
    // m_free
    // m_track_barq_cb
    // m_create_fa_cb

    BARQ_ASSERT(c.app_id);
    BARQ_ASSERT(c.user_id);
    BARQ_ASSERT(c.access_token_cb);
    BARQ_ASSERT(c.refresh_token_cb);
    BARQ_ASSERT(c.state_cb);
    BARQ_ASSERT(c.atrr_cb);
    BARQ_ASSERT(c.sync_manager_cb);
    BARQ_ASSERT(c.request_log_out_cb);
    BARQ_ASSERT(c.request_refresh_location_cb);
    BARQ_ASSERT(c.request_access_token_cb);

    return wrap_err([&]() {
        auto capi_user = std::make_shared<CAPIAppUser>(c.app_id, c.user_id);
        capi_user->m_userdata = c.userdata;
        capi_user->m_free = c.free_func;
        capi_user->m_access_token_cb = c.access_token_cb;
        capi_user->m_refresh_token_cb = c.refresh_token_cb;
        capi_user->m_state_cb = c.state_cb;
        capi_user->m_atrr_cb = c.atrr_cb;
        capi_user->m_sync_manager_cb = c.sync_manager_cb;
        capi_user->m_request_log_out_cb = c.request_log_out_cb;
        capi_user->m_request_refresh_location_cb = c.request_refresh_location_cb;
        capi_user->m_request_access_token_cb = c.request_access_token_cb;
        capi_user->m_track_barq_cb = c.track_barq_cb;
        capi_user->m_create_fa_cb = c.create_fa_cb;

        return new barq_user_t(std::move(capi_user));
    });
}

BARQ_API barq_sync_manager_t* barq_sync_manager_create(const barq_sync_client_config_t* config)
{
    return wrap_err([&]() {
        auto manager = SyncManager::create(*config);
        return new barq_sync_manager_t(std::move(manager));
    });
}

BARQ_API void barq_sync_manager_set_route(const barq_sync_manager_t* manager, const char* route, bool is_verified)
{
    BARQ_ASSERT(manager);
    (*manager)->set_sync_route(route, is_verified);
}
} // namespace barq::c_api
