////////////////////////////////////////////////////////////////////////////
//
// Copyright 2016 Realm Inc.
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

#ifndef BARQ_OS_SYNC_USER_HPP
#define BARQ_OS_SYNC_USER_HPP

#include <barq/object-store/sync/jwt.hpp>
#include <barq/util/functional.hpp>

#include <memory>
#include <string>
#include <vector>

namespace barq {
namespace networking {
struct NetworkError;
} // namespace networking
class SyncManager;
class SyncSession;

enum class SyncFileAction {
    // The Barq files at the given directory will be deleted.
    DeleteBarq,
    // The Barq file will be copied to a 'recovery' directory, and the original Barq files will be deleted.
    BackUpThenDeleteBarq
};

class SyncUser {
public:
    virtual ~SyncUser() = default;
    bool is_logged_in() const
    {
        return state() == State::LoggedIn;
    }

    enum class State {
        // changing these is a file-format breaking change
        LoggedOut = 0,
        LoggedIn = 1,
        Removed = 2,
    };

    /// Server-supplied unique id for this user.
    virtual std::string user_id() const noexcept = 0;
    /// App id which this user is associated with
    virtual std::string app_id() const noexcept = 0;
    /// Legacy uuids attached to this user. Only applicable to networking::User.
    virtual std::vector<std::string> legacy_identities() const
    {
        return {};
    }

    virtual std::string access_token() const = 0;
    virtual std::string refresh_token() const = 0;
    virtual State state() const = 0;

    /// Checks the expiry on the access token against the local time and if it is invalid or expires soon, returns
    /// true.
    virtual bool access_token_refresh_required() const = 0;

    virtual SyncManager* sync_manager() = 0;

    using CompletionHandler = util::UniqueFunction<void(std::optional<networking::NetworkError>)>;
    // The sync server has told the client to log out the user
    // No completion handler as the user is already logged out server-side
    virtual void request_log_out() = 0;
    // The sync server has told the client to refresh the user's location
    virtual void request_refresh_location(CompletionHandler&&) = 0;
    // The sync server has told the client to refresh the user's access token
    virtual void request_access_token(CompletionHandler&&) = 0;

    // Called whenever a Barq is opened with this user to enable deleting them
    // when the user is removed
    virtual void track_barq(std::string_view path) = 0;
    // if the action is BackUpThenDeleteBarq, the path where it was backed up is returned
    virtual std::string create_file_action(SyncFileAction action, std::string_view original_path,
                                           std::optional<std::string> requested_recovery_dir) = 0;
};

} // namespace barq

#endif // BARQ_OS_SYNC_USER_HPP
