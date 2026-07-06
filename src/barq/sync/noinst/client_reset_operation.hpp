/*************************************************************************
 *
 * Copyright 2021 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#ifndef BARQ_NOINST_CLIENT_RESET_OPERATION_HPP
#define BARQ_NOINST_CLIENT_RESET_OPERATION_HPP

#include <barq/db.hpp>
#include <barq/util/functional.hpp>
#include <barq/util/logger.hpp>
#include <barq/sync/client_base.hpp>
#include <barq/sync/config.hpp>
#include <barq/sync/protocol.hpp>

namespace barq::sync {
class SubscriptionStore;
}

namespace barq::_impl::client_reset {
using CallbackBeforeType = util::UniqueFunction<VersionID()>;
using CallbackAfterType = util::UniqueFunction<void(VersionID, bool)>;

std::string get_fresh_path_for(const std::string& barq_path);
bool is_fresh_path(const std::string& barq_path);

bool perform_client_reset(util::Logger& logger, DB& db, sync::ClientReset&& reset_config,
                          sync::SubscriptionStore* sub_store);

} // namespace barq::_impl::client_reset

#endif // BARQ_NOINST_CLIENT_RESET_OPERATION_HPP
