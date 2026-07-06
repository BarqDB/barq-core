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

#include <catch2/catch_all.hpp>

#include "util/event_loop.hpp"
#include "util/test_file.hpp"
#include "util/test_path.hpp"
#include "util/test_utils.hpp"

#include <barq/object-store/binding_context.hpp>
#include <barq/object-store/impl/barq_coordinator.hpp>
#include <barq/object-store/keypath_helpers.hpp>
#include <barq/object-store/object_schema.hpp>
#include <barq/object-store/object_store.hpp>
#include <barq/object-store/property.hpp>
#include <barq/object-store/results.hpp>
#include <barq/object-store/schema.hpp>
#include <barq/object-store/thread_safe_reference.hpp>
#include <barq/object-store/util/scheduler.hpp>

#include <barq/db.hpp>

#if BARQ_ENABLE_SYNC
#include <barq/object-store/sync/async_open_task.hpp>
#endif

#include <barq/util/fifo_helper.hpp>
#include <barq/util/scope_exit.hpp>

using namespace barq;

TEST_CASE("Automated backup", "[backup]") {
    TestFile config;
    std::string copy_from_file_name = test_util::get_test_resource_path() + "test_backup-olden-and-golden.barq";
    config.path = test_util::get_test_path_prefix() + "test_backup.barq";
    config.encryption_key.clear();
    REQUIRE(util::File::exists(copy_from_file_name));
    util::File::copy(copy_from_file_name, config.path);
    REQUIRE(util::File::exists(config.path));
    // backup name must reflect version of old barq file (which is v6)
    std::string backup_path = test_util::get_test_path_prefix() + "test_backup.v20.backup.barq";
    std::string backup_log = test_util::get_test_path_prefix() + "test_backup.barq.backup-log";
    util::File::try_remove(backup_path);
    util::File::try_remove(backup_log);

    SECTION("Backup enabled will produce correctly named backup") {
        config.backup_at_file_format_change = true;
        auto barq = Barq::get_shared_barq(config);
        REQUIRE(util::File::exists(backup_path));
        REQUIRE(util::File::exists(backup_log));
    }

    SECTION("Backup disabled produces no backup") {
        config.backup_at_file_format_change = false;
        auto barq = Barq::get_shared_barq(config);
        REQUIRE(!util::File::exists(backup_path));
        REQUIRE(!util::File::exists(backup_log));
    }
}
