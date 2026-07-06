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

#include <util/sync/sync_test_utils.hpp>

#include <util/test_file.hpp>

#include <barq/object-store/binding_context.hpp>
#include <barq/object-store/object_store.hpp>
#include <barq/object-store/impl/object_accessor_impl.hpp>
#include <barq/object-store/sync/async_open_task.hpp>

#include <barq/sync/client_base.hpp>
#include <barq/sync/protocol.hpp>
#include <barq/sync/noinst/client_history_impl.hpp>
#include <barq/sync/noinst/client_reset.hpp>

#include <barq/util/base64.hpp>
#include <barq/util/hex_dump.hpp>
#include <barq/util/sha_crypto.hpp>

#include <chrono>

namespace barq {

std::ostream& operator<<(std::ostream& os, util::Optional<networking::NetworkError> error)
{
    if (!error) {
        os << "(none)";
    }
    else {
        os << "NetworkError(error_code=" << error->code() << ", server_error=" << error->server_error
           << ", http_status_code=" << error->additional_status_code.value_or(0) << ", message=\"" << error->reason()
           << "\", link_to_server_logs=\"" << error->link_to_server_logs << "\")";
    }
    return os;
}

bool ReturnsTrueWithinTimeLimit::match(util::FunctionRef<bool()> condition) const
{
    const auto wait_start = std::chrono::steady_clock::now();
    const auto delay = TEST_TIMEOUT_EXTRA > 0 ? m_max_ms + std::chrono::seconds(TEST_TIMEOUT_EXTRA) : m_max_ms;
    bool predicate_returned_true = false;
    util::EventLoop::main().run_until([&] {
        if (std::chrono::steady_clock::now() - wait_start > delay) {
            util::format("ReturnsTrueWithinTimeLimit exceeded %1 ms", delay.count());
            return true;
        }
        auto ret = condition();
        if (ret) {
            predicate_returned_true = true;
        }
        return ret;
    });

    return predicate_returned_true;
}

void timed_wait_for(util::FunctionRef<bool()> condition, std::chrono::milliseconds max_ms)
{
    const auto wait_start = std::chrono::steady_clock::now();
    const auto delay = TEST_TIMEOUT_EXTRA > 0 ? max_ms + std::chrono::seconds(TEST_TIMEOUT_EXTRA) : max_ms;
    util::EventLoop::main().run_until([&] {
        if (std::chrono::steady_clock::now() - wait_start > delay) {
            throw std::runtime_error(util::format("timed_wait_for exceeded %1 ms", delay.count()));
        }
        return condition();
    });
}

void timed_sleeping_wait_for(util::FunctionRef<bool()> condition, std::chrono::milliseconds max_ms,
                             std::chrono::milliseconds sleep_ms)
{
    const auto wait_start = std::chrono::steady_clock::now();
    const auto delay = TEST_TIMEOUT_EXTRA > 0 ? max_ms + std::chrono::seconds(TEST_TIMEOUT_EXTRA) : max_ms;
    while (!condition()) {
        if (std::chrono::steady_clock::now() - wait_start > delay) {
            throw std::runtime_error(util::format("timed_sleeping_wait_for exceeded %1 ms", delay.count()));
        }
        std::this_thread::sleep_for(sleep_ms);
    }
}

auto do_hash = [](const std::string& name) -> std::string {
    std::array<unsigned char, 32> hash;
    util::sha256(name.data(), name.size(), hash.data());
    return util::hex_dump(hash.data(), hash.size(), "");
};

ExpectedBarqPaths::ExpectedBarqPaths(const std::string& base_path, const std::string& app_id,
                                       const std::string& identity, const std::vector<std::string>& legacy_identities,
                                       const std::string& partition)
{
    // This is copied from SyncManager.cpp string_from_partition() in order to prevent
    // us changing that function and therefore breaking user's existing paths unknowingly.
    std::string cleaned_partition = "p_" + do_hash(partition);

    std::string clean_name = cleaned_partition;
    std::string cleaned_app_id = util::make_percent_encoded_string(app_id);
    const auto manager_path = fs::path{base_path}.make_preferred() / "barq-sync" / cleaned_app_id;
    const auto preferred_name = manager_path / identity / clean_name;
    current_preferred_path = preferred_name.string() + ".barq";
    fallback_hashed_path = (manager_path / do_hash(preferred_name.string())).string() + ".barq";

    if (legacy_identities.size() < 1)
        return;
    auto& local_identity = legacy_identities[0];
    legacy_sync_directories_to_make.push_back((manager_path / local_identity).string());
    std::string encoded_partition = util::make_percent_encoded_string(partition);
    legacy_local_id_path = (manager_path / local_identity / encoded_partition).concat(".barq").string();
    auto dir_builder = manager_path / "barq-object-server";
    legacy_sync_directories_to_make.push_back(dir_builder.string());
    dir_builder /= local_identity;
    legacy_sync_directories_to_make.push_back(dir_builder.string());
    legacy_sync_path = (dir_builder / cleaned_partition).string();
}

std::string unquote_string(std::string_view possibly_quoted_string)
{
    if (possibly_quoted_string.size() > 0) {
        auto check_char = possibly_quoted_string.front();
        if (check_char == '"' || check_char == '\'') {
            possibly_quoted_string.remove_prefix(1);
        }
    }
    if (possibly_quoted_string.size() > 0) {
        auto check_char = possibly_quoted_string.back();
        if (check_char == '"' || check_char == '\'') {
            possibly_quoted_string.remove_suffix(1);
        }
    }
    return std::string{possibly_quoted_string};
}

#if BARQ_ENABLE_SYNC

sync::SubscriptionSet subscribe_to_all(Barq& barq)
{
    auto mut_subs = barq.get_latest_subscription_set().make_mutable_copy();
    auto& group = barq.read_group();
    for (auto key : group.get_table_keys()) {
        if (group.table_is_public(key)) {
            auto table = group.get_table(key);
            if (table->get_table_type() == Table::Type::TopLevel) {
                mut_subs.insert_or_assign(table->where());
            }
        }
    }
    return std::move(mut_subs).commit();
}

void subscribe_to_all_and_bootstrap(Barq& barq)
{
    auto subs = subscribe_to_all(barq);
    subs.get_state_change_notification(sync::SubscriptionSet::State::Complete).get();
    wait_for_download(barq);
}

void wait_for_advance(Barq& barq)
{
    struct Context : BindingContext {
        Barq& barq;
        DB::version_type target_version;
        bool& done;
        Context(Barq& barq, bool& done)
            : barq(barq)
            , target_version(*barq.latest_snapshot_version())
            , done(done)
        {
            // Are we already there...
            if (barq.read_transaction_version().version >= target_version) {
                done = true;
            }
        }

        void did_change(std::vector<ObserverState> const&, std::vector<void*> const&, bool) override
        {
            if (barq.read_transaction_version().version >= target_version) {
                done = true;
            }
        }
    };

    bool done = false;
    barq.m_binding_context = std::make_unique<Context>(barq, done);
    timed_wait_for([&] {
        return done;
    });
    barq.m_binding_context = nullptr;
}

StatusWith<std::shared_ptr<Barq>> async_open_barq(const Barq::Config& config)
{
    auto task = Barq::get_synchronized_barq(config);
    auto sw = task->start().get_no_throw();
    if (sw.is_ok())
        return Barq::get_shared_barq(std::move(sw.get_value()));
    return sw.get_status();
}

std::shared_ptr<Barq> successfully_async_open_barq(const Barq::Config& config)
{
    auto status = async_open_barq(config);
    REQUIRE(status.is_ok());
    return status.get_value();
}

#endif // BARQ_ENABLE_SYNC

class TestHelper {
public:
    static DBRef& get_db(SharedBarq const& shared_barq)
    {
        return Barq::Internal::get_db(*shared_barq);
    }
};

namespace reset_utils {

Obj create_object(Barq& barq, StringData object_type, util::Optional<ObjectId> primary_key,
                  util::Optional<Partition> partition)
{
    auto table = barq::ObjectStore::table_for_object_type(barq.read_group(), object_type);
    REQUIRE(table);
    FieldValues values = {};
    if (partition) {
        ColKey col = table->get_column_key(partition->property_name);
        BARQ_ASSERT(col);
        values.insert(col, Mixed{partition->value});
    }
    return table->create_object_with_primary_key(primary_key ? *primary_key : ObjectId::gen(), std::move(values));
}

namespace {

TableRef get_table(Barq& barq, StringData object_type)
{
    return barq::ObjectStore::table_for_object_type(barq.read_group(), object_type);
}

// Run through the client reset steps manually without involving a sync server.
// Useful for speed and when integration testing is not available on a platform.
struct FakeLocalClientReset : public TestClientReset {
    FakeLocalClientReset(const Barq::Config& local_config, const Barq::Config& remote_config)
        : TestClientReset(local_config, remote_config)
    {
        BARQ_ASSERT(m_local_config.sync_config);
        m_mode = m_local_config.sync_config->client_resync_mode;
        BARQ_ASSERT(m_mode == ClientResyncMode::DiscardLocal || m_mode == ClientResyncMode::Recover);
        // Turn off real sync. But we still need a SyncClientHistory for recovery mode so fake it.
        m_local_config.sync_config = {};
        m_remote_config.sync_config = {};
        m_local_config.force_sync_history = true;
        m_remote_config.force_sync_history = true;
        m_local_config.in_memory = true;
        m_local_config.encryption_key = std::vector<char>();
        m_remote_config.in_memory = true;
        m_remote_config.encryption_key = std::vector<char>();
    }

    void run() override
    {
        m_did_run = true;
        auto local_barq = Barq::get_shared_barq(m_local_config);
        if (m_on_setup) {
            local_barq->begin_transaction();
            m_on_setup(local_barq);
            local_barq->commit_transaction();

            // Update the sync history to mark this initial setup state as if it
            // has been uploaded so that it doesn't replay during recovery.
            auto history_local =
                dynamic_cast<sync::ClientHistory*>(local_barq->read_group().get_replication()->_get_history_write());
            BARQ_ASSERT(history_local);
            sync::version_type current_version;
            sync::SaltedFileIdent file_ident;
            sync::SyncProgress progress;
            history_local->get_status(current_version, file_ident, progress);
            progress.upload.client_version = current_version;
            progress.upload.last_integrated_server_version = current_version;
            sync::VersionInfo info_out;
            history_local->set_sync_progress(progress, 0, info_out);
        }
        {
            local_barq->begin_transaction();
            auto obj = create_object(*local_barq, "object", m_pk_driving_reset);
            auto col = obj.get_table()->get_column_key("value");
            obj.set(col, 1);
            obj.set(col, 2);
            obj.set(col, 3);
            local_barq->commit_transaction();

            local_barq->begin_transaction();
            obj.set(col, 4);
            if (m_make_local_changes) {
                m_make_local_changes(local_barq);
            }
            local_barq->commit_transaction();
            if (m_on_post_local) {
                m_on_post_local(local_barq);
            }
        }

        {
            auto remote_barq = Barq::get_shared_barq(m_remote_config);
            remote_barq->begin_transaction();
            if (m_on_setup) {
                m_on_setup(remote_barq);
            }

            // fake a sync by creating an object with the same pk
            create_object(*remote_barq, "object", m_pk_driving_reset);

            for (int i = 0; i < 2; ++i) {
                auto table = get_table(*remote_barq, "object");
                auto col = table->get_column_key("value");
                table->begin()->set(col, i + 5);
            }

            if (m_make_remote_changes) {
                m_make_remote_changes(remote_barq);
            }
            remote_barq->commit_transaction();

            auto local_db = TestHelper::get_db(local_barq);
            auto logger = util::Logger::get_default_logger();
            sync::ClientReset reset_config{m_mode,
                                           TestHelper::get_db(remote_barq),
                                           {ErrorCodes::SyncClientResetRequired, "Bad client file ident"}};

            using _impl::client_reset::perform_client_reset_diff;
            perform_client_reset_diff(*local_db, reset_config, *logger, nullptr);

            remote_barq->close();
            if (m_on_post_reset) {
                m_on_post_reset(local_barq);
            }
        }
    }

private:
    ClientResyncMode m_mode;
};
} // anonymous namespace

#if BARQ_ENABLE_SYNC

#endif // BARQ_ENABLE_SYNC


TestClientReset::TestClientReset(const Barq::Config& local_config, const Barq::Config& remote_config)
    : m_local_config(local_config)
    , m_remote_config(remote_config)
{
}
TestClientReset::~TestClientReset()
{
    // make sure we didn't forget to call run()
    BARQ_ASSERT(m_did_run || !(m_make_local_changes || m_make_remote_changes || m_on_post_local || m_on_post_reset));
}

TestClientReset* TestClientReset::setup(Callback&& on_setup)
{
    m_on_setup = std::move(on_setup);
    return this;
}
TestClientReset* TestClientReset::make_local_changes(Callback&& changes_local)
{
    m_make_local_changes = std::move(changes_local);
    return this;
}
TestClientReset* TestClientReset::populate_initial_object(InitialObjectCallback&& callback)
{
    m_populate_initial_object = std::move(callback);
    return this;
}

TestClientReset* TestClientReset::make_remote_changes(Callback&& changes_remote)
{
    m_make_remote_changes = std::move(changes_remote);
    return this;
}
TestClientReset* TestClientReset::on_post_local_changes(Callback&& post_local)
{
    m_on_post_local = std::move(post_local);
    return this;
}
TestClientReset* TestClientReset::on_post_reset(Callback&& post_reset)
{
    m_on_post_reset = std::move(post_reset);
    return this;
}
TestClientReset* TestClientReset::set_development_mode(bool)
{
    return this;
}

void TestClientReset::set_pk_of_object_driving_reset(const ObjectId& pk)
{
    m_pk_driving_reset = pk;
}

ObjectId TestClientReset::get_pk_of_object_driving_reset() const
{
    return m_pk_driving_reset;
}

void TestClientReset::disable_wait_for_reset_completion()
{
    m_wait_for_reset_completion = false;
}

std::unique_ptr<TestClientReset> make_fake_local_client_reset(const Barq::Config& local_config,
                                                              const Barq::Config& remote_config)
{
    return std::make_unique<FakeLocalClientReset>(local_config, remote_config);
}

} // namespace reset_utils

} // namespace barq
