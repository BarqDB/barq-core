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

#include "util/test_file.hpp"

#include "util/test_utils.hpp"
#include "util/sync/sync_test_utils.hpp"
#include "../util/crypt_key.hpp"
#include "../util/test_path.hpp"
#include "util/sync/sync_test_utils.hpp"

#include <barq/db.hpp>
#include <barq/disable_sync_to_disk.hpp>
#include <barq/history.hpp>
#include <barq/string_data.hpp>
#include <barq/object-store/impl/barq_coordinator.hpp>
#include <barq/util/base64.hpp>
#include <barq/util/file.hpp>

#if BARQ_ENABLE_SYNC
#include <barq/object-store/sync/sync_manager.hpp>
#include <barq/object-store/sync/sync_session.hpp>
#include <barq/object-store/sync/sync_user.hpp>
#include <barq/object-store/schema.hpp>
#endif

#include <cstdlib>
#include <iostream>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>

inline static int mkstemp(char* _template)
{
    return _open(_mktemp(_template), _O_CREAT | _O_TEMPORARY, _S_IREAD | _S_IWRITE);
}
#else
#include <unistd.h>
#endif

#if BARQ_HAVE_CLANG_FEATURE(thread_sanitizer)
#include <condition_variable>
#include <functional>
#include <thread>
#include <map>
#endif

using namespace barq;

TestFile::TestFile()
{
    disable_sync_to_disk();
    m_temp_dir = util::make_temp_dir();
    path = (fs::path(m_temp_dir) / "barq.XXXXXX").string();
    if (const char* crypt_key = test_util::crypt_key()) {
        encryption_key = std::vector<char>(crypt_key, crypt_key + 64);
    }
    int fd = mkstemp(path.data());
    if (fd < 0) {
        int err = errno;
        throw std::system_error(err, std::system_category());
    }
#ifdef _WIN32
    _close(fd);
    _unlink(path.c_str());
#else // POSIX
    close(fd);
    unlink(path.c_str());
#endif

    schema_version = 0;
}

TestFile::~TestFile()
{
    if (!m_persist) {
        try {
            util::Logger::get_default_logger()->debug("~TestFile() removing '%1' and '%2'", path, m_temp_dir);
            util::File::try_remove(path);
            util::try_remove_dir_recursive(m_temp_dir);
        }
        catch (const std::exception& e) {
            util::Logger::get_default_logger()->warn("~TestFile() cleanup failed for '%1': %2", path, e.what());
            // clean up is best effort, ignored.
        }
    }
}

DBOptions TestFile::options() const
{
    DBOptions options;
    options.durability = in_memory ? DBOptions::Durability::MemOnly : DBOptions::Durability::Full;
    return options;
}

InMemoryTestFile::InMemoryTestFile()
{
    in_memory = true;
    schema_version = 0;
    encryption_key = std::vector<char>();
}

DBOptions InMemoryTestFile::options() const
{
    DBOptions options;
    options.durability = DBOptions::Durability::MemOnly;
    return options;
}

#if BARQ_ENABLE_SYNC

static const std::string fake_refresh_token = ENCODE_FAKE_JWT("not_a_real_token");
static const std::string fake_access_token = ENCODE_FAKE_JWT("also_not_real");
static const std::string fake_device_id = "123400000000000000000000";

SyncTestFile::SyncTestFile(TestSyncManager& tsm, std::string name, std::string user_name)
    : SyncTestFile(tsm.fake_user(user_name), std::move(name))
{
}

SyncTestFile::SyncTestFile(std::shared_ptr<SyncUser> user, std::string partition, util::Optional<Schema> schema)
{
    BARQ_ASSERT(user);
    sync_config = std::make_shared<barq::SyncConfig>(user, std::move(partition));
    sync_config->stop_policy = SyncSessionStopPolicy::Immediately;
    sync_config->error_handler = [](std::shared_ptr<SyncSession>, SyncError error) {
        util::format(std::cerr, "An unexpected sync error was caught by the default SyncTestFile handler: '%1'\n",
                     error.status);
        abort();
    };
    schema_version = 1;
    this->schema = std::move(schema);
    schema_mode = SchemaMode::AdditiveExplicit;
}

SyncTestFile::SyncTestFile(std::shared_ptr<SyncUser> user, std::string partition,
                           barq::util::Optional<barq::Schema> schema,
                           std::function<SyncSessionErrorHandler>&& error_handler)
{
    BARQ_ASSERT(user);
    sync_config = std::make_shared<barq::SyncConfig>(user, std::move(partition));
    sync_config->stop_policy = SyncSessionStopPolicy::Immediately;
    sync_config->error_handler = std::move(error_handler);
    schema_version = 1;
    this->schema = std::move(schema);
    schema_mode = SchemaMode::AdditiveExplicit;
}

SyncTestFile::SyncTestFile(std::shared_ptr<barq::SyncUser> user, barq::Schema _schema, SyncConfig::FLXSyncEnabled)
{
    BARQ_ASSERT(user);
    sync_config = std::make_shared<barq::SyncConfig>(user, SyncConfig::FLXSyncEnabled{});
    sync_config->stop_policy = SyncSessionStopPolicy::Immediately;
    sync_config->error_handler = [](std::shared_ptr<SyncSession> session, SyncError error) {
        util::format(std::cerr,
                     "An unexpected sync error was caught by the default SyncTestFile handler: '%1' for '%2'",
                     error.status, session->path());
        abort();
    };
    schema_version = 0;
    schema = _schema;
    schema_mode = SchemaMode::AdditiveExplicit;
}

SyncTestFile::SyncTestFile(TestSyncManager& tsm, std::string partition, Schema schema)
    : SyncTestFile(tsm.fake_user("test"), std::move(partition), std::move(schema))
{
}

// MARK: - SyncServer
SyncServer::SyncServer(const SyncServer::Config& config)
    : m_local_root_dir(config.local_dir.empty() ? util::make_temp_dir() : config.local_dir)
    , m_server(m_local_root_dir, util::none, ([&] {
                   using namespace std::literals::chrono_literals;

                   m_logger = util::Logger::get_default_logger();

                   sync::Server::Config c;
                   c.logger = m_logger;
                   c.token_expiration_clock = this;
                   c.listen_address = "127.0.0.1";
                   c.disable_sync_to_disk = true;
                   c.ssl = config.ssl;
                   if (c.ssl) {
                       c.ssl_certificate_path = test_util::get_test_resource_path() + "test_util_network_ssl_ca.pem";
                       c.ssl_certificate_key_path =
                           test_util::get_test_resource_path() + "test_util_network_ssl_key.pem";
                   }

                   return c;
               })())
{
    m_server.start();
    m_url = util::format("%1://127.0.0.1:%2", config.ssl ? "wss" : "ws", m_server.listen_endpoint().port());
    if (config.start_immediately)
        start();
}

SyncServer::~SyncServer()
{
    stop();
}

void SyncServer::start()
{
    BARQ_ASSERT(!m_thread.joinable());
    m_thread = std::thread([this] {
        m_server.run();
    });
}

void SyncServer::stop()
{
    m_server.stop();
    if (m_thread.joinable())
        m_thread.join();
}

std::string SyncServer::url_for_barq(StringData barq_name) const
{
    return util::format("%1/%2", m_url, barq_name);
}

int SyncServer::port() const
{
    return static_cast<int>(m_server.listen_endpoint().port());
}

struct WaitForSessionState {
    std::condition_variable cv;
    std::mutex mutex;
    bool complete = false;
    Status status = Status::OK();
};

static Status wait_for_session(Barq& barq, void (SyncSession::*fn)(util::UniqueFunction<void(Status)>&&),
                               std::chrono::seconds timeout)
{
    auto shared_state = std::make_shared<WaitForSessionState>();
    auto& session = *barq.sync_session();
    auto delay = TEST_TIMEOUT_EXTRA > 0 ? timeout + std::chrono::seconds(TEST_TIMEOUT_EXTRA) : timeout;
    (session.*fn)([weak_state = std::weak_ptr<WaitForSessionState>(shared_state)](Status s) {
        auto shared_state = weak_state.lock();
        if (!shared_state) {
            return;
        }
        std::lock_guard<std::mutex> lock(shared_state->mutex);
        shared_state->complete = true;
        shared_state->status = s;
        shared_state->cv.notify_one();
    });
    std::unique_lock<std::mutex> lock(shared_state->mutex);
    bool completed = shared_state->cv.wait_for(lock, delay, [&]() {
        return shared_state->complete == true;
    });
    if (!completed) {
        throw std::runtime_error(util::format("wait_for_session() exceeded %1 s", delay.count()));
    }
    return shared_state->status;
}

bool wait_for_upload(Barq& barq, std::chrono::seconds timeout)
{
    return !wait_for_session(barq, &SyncSession::wait_for_upload_completion, timeout).is_ok();
}

bool wait_for_download(Barq& barq, std::chrono::seconds timeout)
{
    return !wait_for_session(barq, &SyncSession::wait_for_download_completion, timeout).is_ok();
}



// MARK: - TestSyncManager

TestSyncManager::Config::Config() {}

TestSyncManager::TestSyncManager(const Config& config, const SyncServer::Config& sync_server_config)
    : m_sync_manager(SyncManager::create(SyncClientConfig()))
    , m_sync_server(sync_server_config)
    , m_base_file_path(config.base_path.empty() ? util::make_temp_dir() : config.base_path)
    , m_should_teardown_test_directory(config.should_teardown_test_directory)
{
    util::try_make_dir(m_base_file_path);

    m_sync_manager->set_sync_route(m_sync_server.base_url() + "/barq-sync", true);
    if (config.start_sync_client) {
        m_sync_manager->get_sync_client();
    }
}

TestSyncManager::~TestSyncManager()
{
    if (m_should_teardown_test_directory) {
        if (!m_base_file_path.empty() && util::File::exists(m_base_file_path)) {
            try {
                m_sync_manager->tear_down_for_testing();
                util::try_remove_dir_recursive(m_base_file_path);
            }
            catch (const std::exception& ex) {
                std::cerr << ex.what() << "\n";
            }
        }
    }
}

std::shared_ptr<TestUser> TestSyncManager::fake_user(const std::string& name)
{
    auto user = std::make_shared<TestUser>(name, m_sync_manager);
    user->m_access_token = fake_access_token;
    user->m_refresh_token = fake_refresh_token;
    return user;
}

#endif // BARQ_ENABLE_SYNC

#if BARQ_HAVE_CLANG_FEATURE(thread_sanitizer)
// MARK: - TsanNotifyWorker
// A helper which synchronously runs on_change() on a fixed background thread
// so that ThreadSanitizer can potentially detect issues
// This deliberately uses an unsafe spinlock for synchronization to ensure that
// the code being tested has to supply all required safety
static class TsanNotifyWorker {
public:
    TsanNotifyWorker()
    {
        m_thread = std::thread([&] {
            work();
        });
    }

    void work()
    {
        while (true) {
            auto value = m_signal.load(std::memory_order_relaxed);
            if (value == 0 || value == 1)
                continue;
            if (value == 2)
                return;

            if (value & 1) {
                // Synchronize on the first handover of a given coordinator.
                value &= ~1;
                m_signal.load();
            }

            auto c = reinterpret_cast<_impl::BarqCoordinator*>(value);
            c->on_change();
            m_signal.store(1, std::memory_order_relaxed);
        }
    }

    ~TsanNotifyWorker()
    {
        m_signal = 2;
        m_thread.join();
    }

    void on_change(const std::shared_ptr<_impl::BarqCoordinator>& c)
    {
        auto& it = m_published_coordinators[c.get()];
        if (it.lock()) {
            m_signal.store(reinterpret_cast<uintptr_t>(c.get()), std::memory_order_relaxed);
        }
        else {
            // Synchronize on the first handover of a given coordinator.
            it = c;
            m_signal = reinterpret_cast<uintptr_t>(c.get()) | 1;
        }

        while (m_signal.load(std::memory_order_relaxed) != 1)
            ;
    }

private:
    std::atomic<uintptr_t> m_signal{0};
    std::thread m_thread;
    std::map<_impl::BarqCoordinator*, std::weak_ptr<_impl::BarqCoordinator>> m_published_coordinators;
} s_worker;

void on_change_but_no_notify(Barq& barq)
{
    s_worker.on_change(_impl::BarqCoordinator::get_existing_coordinator(barq.config().path));
}

void advance_and_notify(Barq& barq)
{
    on_change_but_no_notify(barq);
    barq.notify();
}

#else // BARQ_HAVE_CLANG_FEATURE(thread_sanitizer)

void on_change_but_no_notify(Barq& barq)
{
    _impl::BarqCoordinator::get_coordinator(barq.config().path)->on_change();
}

void advance_and_notify(Barq& barq)
{
    on_change_but_no_notify(barq);
    barq.notify();
}
#endif
