#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#if defined(BARQ_SYNC_SERVER_BINARY) && !defined(_WIN32) && !BARQ_ANDROID && !BARQ_IOS
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(BARQ_SYNC_SERVER_BINARY) && !defined(_WIN32) && !BARQ_ANDROID && !BARQ_IOS
extern char** environ;

const char g_live_rules_seed_token[] =
    "eyJpZGVudGl0eSI6InNlZWQiLCJhZG1pbiI6dHJ1ZSwidGltZXN0YW1wIjoxNDU1NTMwNjE0LCJleHBpcmVzIjpudWxsLCJhcHBfaWQiOiJ0ZXN0IiwicGF0aCI6InRlbmFudC1hL21haW4iLCJhY2Nlc3MiOlsiZG93bmxvYWQiLCJ1cGxvYWQiLCJtYW5hZ2UiXX0=";
const char g_live_rules_user_token[] =
    "eyJpZGVudGl0eSI6InVzZXJfMCIsImFkbWluIjpmYWxzZSwidGltZXN0YW1wIjoxNDU1NTMwNjE0LCJleHBpcmVzIjpudWxsLCJhcHBfaWQiOiJ0ZXN0IiwicGF0aCI6InRlbmFudC1hL21haW4iLCJhY2Nlc3MiOlsiZG93bmxvYWQiLCJ1cGxvYWQiLCJtYW5hZ2UiXX0=";
#endif

#include <barq.hpp>
#include <barq/chunked_binary.hpp>
#include <barq/data_type.hpp>
#include <barq/dictionary.hpp>
#include <barq/history.hpp>
#include <barq/impl/simulated_failure.hpp>
#include <barq/list.hpp>
#include <barq/sync/binding_callback_thread_observer.hpp>
#include <barq/sync/changeset.hpp>
#include <barq/sync/changeset_encoder.hpp>
#include <barq/sync/client.hpp>
#include <barq/sync/history.hpp>
#include <barq/sync/instructions.hpp>
#include <barq/sync/network/default_socket.hpp>
#include <barq/sync/network/http.hpp>
#include <barq/sync/network/network.hpp>
#include <barq/sync/network/websocket.hpp>
#include <barq/sync/noinst/protocol_codec.hpp>
#include <barq/sync/noinst/server/access_control.hpp>
#include <barq/sync/noinst/server/server.hpp>
#include <barq/sync/noinst/server/server_dir.hpp>
#include <barq/sync/noinst/server/server_history.hpp>
#include <barq/sync/object_id.hpp>
#include <barq/sync/protocol.hpp>
#include <barq/sync/subscriptions.hpp>
#include <barq/sync/transform.hpp>
#include <barq/util/buffer.hpp>
#include <barq/util/features.h>
#include <barq/util/logger.hpp>
#include <barq/util/random.hpp>
#include <barq/util/uri.hpp>
#include <barq/version.hpp>

#include "sync_fixtures.hpp"

#include "test.hpp"
#include "util/demangle.hpp"
#include "util/semaphore.hpp"
#include "util/thread_wrapper.hpp"
#include "util/compare_groups.hpp"

using namespace barq;
using namespace barq::sync;
using namespace barq::test_util;
using namespace barq::fixtures;


// Test independence and thread-safety
// -----------------------------------
//
// All tests must be thread safe and independent of each other. This
// is required because it allows for both shuffling of the execution
// order and for parallelized testing.
//
// In particular, avoid using std::rand() since it is not guaranteed
// to be thread safe. Instead use the API offered in
// `test/util/random.hpp`.
//
// All files created in tests must use the TEST_PATH macro (or one of
// its friends) to obtain a suitable file system path. See
// `test/util/test_path.hpp`.
//
//
// Debugging and the ONLY() macro
// ------------------------------
//
// A simple way of disabling all tests except one called `Foo`, is to
// replace TEST(Foo) with ONLY(Foo) and then recompile and rerun the
// test suite. Note that you can also use filtering by setting the
// environment variable `UNITTEST_FILTER`. See `README.md` for more on
// this.
//
// Another way to debug a particular test, is to copy that test into
// `experiments/testcase.cpp` and then run `sh build.sh
// check-testcase` (or one of its friends) from the command line.


namespace {

using ErrorInfo = SessionErrorInfo;

class TestServerHistoryContext : public _impl::ServerHistory::Context {
public:
    std::mt19937_64& server_history_get_random() noexcept override final
    {
        return m_random;
    }

private:
    std::mt19937_64 m_random;
};

#define TEST_CLIENT_DB(name)                                                                                         \
    SHARED_GROUP_TEST_PATH(name##_path);                                                                             \
    auto name = DB::create(make_client_replication(), name##_path);

template <typename Function>
DB::version_type write_transaction(DBRef db, Function&& function)
{
    WriteTransaction wt(db);
    function(wt);
    return wt.commit();
}

ClientReplication& get_replication(DBRef db)
{
    auto repl = dynamic_cast<ClientReplication*>(db->get_replication());
    BARQ_ASSERT(repl);
    return *repl;
}

ClientHistory& get_history(DBRef db)
{
    return get_replication(db).get_history();
}

const char g_tenant_pbs_user_token[] =
    "eyJpZGVudGl0eSI6InBic191c2VyIiwiYWRtaW4iOmZhbHNlLCJ0aW1lc3RhbXAiOjE0NTU1MzA2MTQs"
    "ImV4cGlyZXMiOm51bGwsImFwcF9pZCI6InRlbmFudC1wYnMiLCJwYXRoIjoicGJzIiwiYWNjZXNzIjpb"
    "ImRvd25sb2FkIiwidXBsb2FkIiwibWFuYWdlIl19"
    ":"
    "ScYHEV1RPldbhtHlLoip6bhxJbOrrDMZXsdsjpPQjpNFjo+I8rfPAJYthlLBEHIHM/cwj/yeCcOYJFnA"
    "fp+yA87jExVJ0HSn0XTlj2M8LP8cYap7F68P309gCQrw0omO7PQcJIukOtXjcYV8of5XH+rKqU2LN8l"
    "66Z1x7kB8vZDPHRw3j4UeaO+tTgF/HqWAQQ4aE7Bx5yZjtG43xi57Og7CbELPAOhV8yBllhRwfbEM/o"
    "fQfuTbTTALYGSsUbFwE07N6A8jfwIw4HKV3F5f8ymz6HPj6/J833ROU/9J6No6CBsdH9j7BDz7WrAMH"
    "HWNLu9CAjOoL9CJU4YboW9dZg==";

const char g_tenant_flx_seed_token[] =
    "eyJpZGVudGl0eSI6ImZseF9zZWVkIiwiYWRtaW4iOmZhbHNlLCJ0aW1lc3RhbXAiOjE0NTU1MzA2MTQs"
    "ImV4cGlyZXMiOm51bGwsImFwcF9pZCI6InRlbmFudC1mbHgiLCJwYXRoIjoiZmx4IiwiYWNjZXNzIjpb"
    "ImRvd25sb2FkIiwidXBsb2FkIiwibWFuYWdlIl19"
    ":"
    "3DpSoDLSeqoQIcv55oUhZdF4+Dei2ASOJwt6zyeU9NWBPuIuB9oXDjbownZLVBHGM/Xey2BpMF9lt1w"
    "r1rZje++b7t2uQsTgQYDicRNcO3VZ1o+IXWbpe5CX4aObBgJ5ZLakTMIOELoG9hjsIvCqaqGEh0iCn"
    "kYSeQAlhNu0Xbw5GZPYV7itLOmKuppMsWS03Dc4fKKsyQO8O0PPGc0tFswobaaL0p4xa8ICcILETmi"
    "7gJJ+pI6o/zouXkmcEEZItUpnfIBd8xQc2vVgkX59qzSE45RZXGxFIkiHzPaiQ4KNGf5rdjsMJa2rT"
    "ySXuOsOSDT4BN+4Oa3803pWavTIRw==";

const char g_tenant_flx_user_token[] =
    "eyJpZGVudGl0eSI6InVzZXJfMCIsImFkbWluIjpmYWxzZSwidGltZXN0YW1wIjoxNDU1NTMwNjE0LCJl"
    "eHBpcmVzIjpudWxsLCJhcHBfaWQiOiJ0ZW5hbnQtZmx4IiwicGF0aCI6ImZseCIsImFjY2VzcyI6WyJk"
    "b3dubG9hZCIsInVwbG9hZCIsIm1hbmFnZSJdfQ=="
    ":"
    "s8slSB6fCJGJiYTEIO8QswZTEna+dM+dwcC04TRLCa7cb7/QobG14t0kkF3dTLSValH643+JPrNtFsl"
    "waqeHf+UPPfgJbdYhtSoG21iU7+dwWyHFIeaMPeQBhcZbcsNS35ERxbRxM5xYMwgBR9BBCs55+hstQ"
    "mbib05YSBH3dpTNhoTMZmDt9761v3rWXNp/+85LJiU9awmXnlRLCH+uovtVf/zAJ81tEXPlqdxNIkT"
    "nlnFEjMptECzAg3/NSOY8K6SjbvivnZRH6oEYrexhE6YyDz9KST4yhouJTSEYTbwBWey7pBcD7mG+5"
    "RuJR6TJado1sQSwK++WsI6qrFT+SA==";

#if defined(BARQ_SYNC_SERVER_BINARY) && !defined(_WIN32) && !BARQ_ANDROID && !BARQ_IOS
class ExternalServerProcess {
public:
    explicit ExternalServerProcess(std::vector<std::string> args)
    {
        int pipe_fds[2];
        if (pipe(pipe_fds) != 0) {
            throw std::runtime_error(util::format("pipe() failed: %1", errno));
        }

        m_stdout_fd = pipe_fds[0];
        int stdout_write_fd = pipe_fds[1];

        posix_spawn_file_actions_t actions;
        if (posix_spawn_file_actions_init(&actions) != 0) {
            close(stdout_write_fd);
            close(m_stdout_fd);
            m_stdout_fd = -1;
            throw std::runtime_error("posix_spawn_file_actions_init() failed");
        }

        posix_spawn_file_actions_adddup2(&actions, stdout_write_fd, STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, m_stdout_fd);
        posix_spawn_file_actions_addclose(&actions, stdout_write_fd);

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (std::string& arg : args) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);

        int ret = posix_spawn(&m_pid, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        close(stdout_write_fd);

        if (ret != 0) {
            close(m_stdout_fd);
            m_stdout_fd = -1;
            throw std::runtime_error(util::format("posix_spawn(%1) failed: %2", args.front(), ret));
        }
    }

    ~ExternalServerProcess()
    {
        stop();
        if (m_stdout_fd != -1) {
            close(m_stdout_fd);
        }
    }

    ExternalServerProcess(const ExternalServerProcess&) = delete;
    ExternalServerProcess& operator=(const ExternalServerProcess&) = delete;

    std::string wait_for_route()
    {
        std::string output;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto route = find_route(output); !route.empty()) {
                return route;
            }

            pollfd pfd;
            pfd.fd = m_stdout_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;

            int ret = poll(&pfd, 1, 100);
            if (ret < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error(util::format("poll() failed: %1", errno));
            }
            if (ret == 0) {
                continue;
            }

            char buffer[512];
            ssize_t n = read(m_stdout_fd, buffer, sizeof(buffer));
            if (n > 0) {
                output.append(buffer, size_t(n)); // Throws
                continue;
            }
            break;
        }

        throw std::runtime_error("barq-server did not print a listening route. Output: " + output);
    }

private:
    static std::string find_route(const std::string& output)
    {
        std::string marker = " listening on "; // Throws
        std::size_t begin = output.find(marker);
        if (begin == std::string::npos) {
            return {};
        }
        begin += marker.size();
        std::size_t end = output.find('\n', begin);
        if (end == std::string::npos) {
            return {};
        }
        return output.substr(begin, end - begin); // Throws
    }

    void stop()
    {
        if (m_pid <= 0) {
            return;
        }

        kill(m_pid, SIGTERM);
        int status = 0;
        for (int i = 0; i < 50; ++i) {
            pid_t ret = waitpid(m_pid, &status, WNOHANG);
            if (ret == m_pid) {
                m_pid = -1;
                return;
            }
            if (ret == -1 && errno != EINTR) {
                m_pid = -1;
                return;
            }
            usleep(100000);
        }

        kill(m_pid, SIGKILL);
        while (waitpid(m_pid, &status, 0) == -1 && errno == EINTR) {
        }
        m_pid = -1;
    }

    pid_t m_pid = -1;
    int m_stdout_fd = -1;
};

class ExternalSyncClient {
public:
    explicit ExternalSyncClient(std::shared_ptr<util::Logger> logger)
        : m_logger(std::move(logger))
        , m_socket_provider(std::make_shared<websocket::DefaultSocketProvider>(
              m_logger, "", nullptr, websocket::DefaultSocketProvider::AutoStart{false}))
    {
        Client::Config config;
        config.socket_provider = m_socket_provider;
        config.logger = m_logger;
        config.reconnect_mode = ReconnectMode::testing;
        config.ping_keepalive_period = 100000000;
        config.pong_keepalive_timeout = 100000000;
        config.fix_up_object_ids = true;
        m_client = std::make_unique<Client>(std::move(config));
        m_socket_provider->start();
    }

    ~ExternalSyncClient()
    {
        m_client->shutdown_and_wait();
        m_socket_provider.reset();
    }

    Client& get() noexcept
    {
        return *m_client;
    }

private:
    std::shared_ptr<util::Logger> m_logger;
    std::shared_ptr<websocket::DefaultSocketProvider> m_socket_provider;
    std::unique_ptr<Client> m_client;
};

Session::Config make_external_cli_session_config(Session::port_type port, const char* token, const char* user_id,
                                                 unit_test::TestContext& test_context)
{
    Session::Config config;
    config.service_identifier = "/barq-sync";
    config.barq_identifier = "/test";
    config.signed_user_token = token;
    config.user_id = user_id;
    config.server_address = "127.0.0.1";
    config.server_port = port;
    config.connection_state_change_listener = [&](ConnectionState state, std::optional<SessionErrorInfo> error_info) {
        if (state != ConnectionState::disconnected) {
            return;
        }
        BARQ_ASSERT(error_info);
        test_context.logger->error("Client disconnect: %1 (is_fatal=%2)", error_info->status, error_info->is_fatal);
        bool client_error_occurred = true;
        CHECK_NOT(client_error_occurred);
    };
    return config;
}

Session::port_type parse_external_cli_server_port(const std::string& route)
{
    std::string suffix = "/barq-sync";
    std::size_t suffix_pos = route.rfind(suffix);
    if (suffix_pos == std::string::npos) {
        throw std::runtime_error("barq-server route is missing /barq-sync: " + route);
    }

    std::size_t colon_pos = route.rfind(':', suffix_pos);
    if (colon_pos == std::string::npos) {
        throw std::runtime_error("barq-server route is missing a port: " + route);
    }

    unsigned long port = std::stoul(route.substr(colon_pos + 1, suffix_pos - colon_pos - 1)); // Throws
    return Session::port_type(port);
}

HTTPResponse call_external_internal_api(Session::port_type port, const std::string& path, const std::string& body,
                                        unit_test::TestContext& test_context)
{
    HTTPRequest request;
    request.method = HTTPMethod::Post;
    request.path = path;
    request.body = body;
    request.headers["Authorization"] = "Bearer test-internal-secret";
    request.headers["Content-Type"] = "application/json";
    request.headers["Content-Length"] = util::to_string(body.size());
    network::Endpoint endpoint{network::make_address("127.0.0.1"), port};
    HTTPRequestClient client(test_context.logger, endpoint, request);
    client.fetch_response();
    return client.get_response();
}
#endif

Changeset get_reciprocal_changeset(ClientHistory& hist, version_type version)
{
    bool is_compressed = false;
    auto data = hist.get_reciprocal_transform(version, is_compressed);
    Changeset reciprocal_changeset;
    ChunkedBinaryInputStream in{data};
    if (is_compressed) {
        size_t total_size;
        auto decompressed = util::compression::decompress_nonportable_input_stream(in, total_size);
        sync::parse_changeset(*decompressed, reciprocal_changeset); // Throws
    }
    else {
        sync::parse_changeset(in, reciprocal_changeset); // Throws
    }
    return reciprocal_changeset;
}

#if !BARQ_MOBILE // the server is not implemented on devices
TEST(Sync_BadVirtualPath)
{
    // NOTE:  This test is only valid for the mock C++ server.
    //  It still passes because it runs against the mock C++ server, but the
    //  Barq Sync server can behave differently.

    TEST_DIR(dir);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);
    TEST_CLIENT_DB(db_3);
    ClientServerFixture fixture(dir, test_context);
    fixture.start();

    int nerrors = 0;

    auto config = [&] {
        Session::Config config;
        config.connection_state_change_listener = [&](ConnectionState state, util::Optional<ErrorInfo> error_info) {
            if (state != ConnectionState::disconnected)
                return;
            BARQ_ASSERT(error_info);
            CHECK_EQUAL(error_info->status, ErrorCodes::BadSyncPartitionValue);
            CHECK(error_info->is_fatal);
            ++nerrors;
            if (nerrors == 3)
                fixture.stop();
        };
        return config;
    };

    Session session_1 = fixture.make_session(db_1, "/test.barq", config());
    Session session_2 = fixture.make_session(db_2, "/../test", config());
    Session session_3 = fixture.make_session(db_3, "test%abc ", config());

    session_1.wait_for_download_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();
    session_3.wait_for_download_complete_or_client_stopped();
    CHECK_EQUAL(nerrors, 3);
}


TEST(Sync_AsyncWaitForUploadCompletion)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db);
    ClientServerFixture fixture(dir, test_context);
    fixture.start();

    Session session = fixture.make_bound_session(db, "/test");

    auto wait = [&] {
        BowlOfStonesSemaphore bowl;
        auto handler = [&](Status status) {
            if (CHECK(status.is_ok()))
                bowl.add_stone();
        };
        session.async_wait_for_upload_completion(handler);
        bowl.get_stone();
    };

    // Empty
    wait();

    // Nonempty
    write_transaction(db, [](WriteTransaction& wt) {
        wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
    });
    wait();

    // Already done
    wait();

    // More
    write_transaction(db, [](WriteTransaction& wt) {
        wt.get_group().add_table_with_primary_key("class_bar", type_Int, "id");
    });
    wait();
}


TEST(Sync_AsyncWaitForUploadCompletionNoPendingLocalChanges)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db);
    ClientServerFixture fixture(dir, test_context);
    fixture.start();

    Session session = fixture.make_bound_session(db, "/test");

    write_transaction(db, [](WriteTransaction& wt) {
        wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
    });

    auto pf = util::make_promise_future<bool>();
    session.async_wait_for_upload_completion(
        [promise = std::move(pf.promise), tr = db->start_read()](Status status) mutable {
            BARQ_ASSERT(status.is_ok());
            tr->advance_read();
            promise.emplace_value(tr->get_history()->no_pending_local_changes(tr->get_version()));
        });
    CHECK(pf.future.get());
}


TEST(Sync_AsyncWaitForDownloadCompletion)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);
    ClientServerFixture fixture(dir, test_context);
    fixture.start();

    auto wait = [&](Session& session) {
        BowlOfStonesSemaphore bowl;
        auto handler = [&](Status status) {
            if (CHECK(status.is_ok()))
                bowl.add_stone();
        };
        session.async_wait_for_download_completion(handler);
        bowl.get_stone();
    };

    // Nothing to download
    Session session_1 = fixture.make_bound_session(db_1, "/test");
    wait(session_1);

    // Again
    wait(session_1);

    // Upload something via session 2
    Session session_2 = fixture.make_bound_session(db_2, "/test");
    write_transaction(db_2, [](WriteTransaction& wt) {
        wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
    });
    session_2.wait_for_upload_complete_or_client_stopped();

    // Wait for session 1 to download it
    wait(session_1);
    {
        ReadTransaction rt_1(db_1);
        ReadTransaction rt_2(db_2);
        CHECK(compare_groups(rt_1, rt_2, *test_context.logger));
    }

    // Again
    wait(session_1);

    // Wait for session 2 to download nothing
    wait(session_2);

    // Upload something via session 1
    write_transaction(db_1, [](WriteTransaction& wt) {
        wt.get_group().add_table_with_primary_key("class_bar", type_Int, "id");
    });
    session_1.wait_for_upload_complete_or_client_stopped();

    // Wait for session 2 to download it
    wait(session_2);
    {
        ReadTransaction rt_1(db_1);
        ReadTransaction rt_2(db_2);
        CHECK(compare_groups(rt_1, rt_2, *test_context.logger));
    }
}


TEST(Sync_AsyncWaitForSyncCompletion)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);
    ClientServerFixture fixture(dir, test_context);
    fixture.start();

    auto wait = [&](Session& session) {
        BowlOfStonesSemaphore bowl;
        auto handler = [&](Status status) {
            if (CHECK(status.is_ok()))
                bowl.add_stone();
        };
        session.async_wait_for_sync_completion(handler);
        bowl.get_stone();
    };

    // Nothing to synchronize
    Session session_1 = fixture.make_bound_session(db_1);
    wait(session_1);

    // Again
    wait(session_1);

    // Generate changes to be downloaded (uploading via session 2)
    Session session_2 = fixture.make_bound_session(db_2);
    write_transaction(db_2, [](WriteTransaction& wt) {
        wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
    });
    session_2.wait_for_upload_complete_or_client_stopped();

    // Generate changes to be uploaded
    write_transaction(db_1, [](WriteTransaction& wt) {
        wt.get_group().add_table_with_primary_key("class_bar", type_Int, "id");
    });

    // Nontrivial synchronization (upload and download required)
    wait(session_1);
    wait(session_2);

    {
        ReadTransaction rt_1(db_1);
        ReadTransaction rt_2(db_2);
        CHECK(compare_groups(rt_1, rt_2, *test_context.logger));
    }
}


TEST(Sync_AsyncWaitCancellation)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db);
    ClientServerFixture fixture(dir, test_context);

    BowlOfStonesSemaphore bowl;
    auto completion_handler = [&](Status status) {
        CHECK_EQUAL(status, ErrorCodes::OperationAborted);
        bowl.add_stone();
    };
    {
        Session session = fixture.make_bound_session(db, "/test");
        session.async_wait_for_upload_completion(completion_handler);
        session.async_wait_for_download_completion(completion_handler);
        session.async_wait_for_sync_completion(completion_handler);
        // Destruction of session cancels wait operations
    }
    fixture.start();
    bowl.get_stone();
    bowl.get_stone();
    bowl.get_stone();
}


TEST(Sync_WaitForUploadCompletion)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db);
    ClientServerFixture fixture{dir, test_context};
    std::string virtual_path = "/test";
    std::string server_path = fixture.map_virtual_to_real_path(virtual_path);
    fixture.start();

    // Empty
    Session session = fixture.make_bound_session(db);
    // Since the Barq is empty, the following wait operation can complete
    // without the client ever having been in contact with the server
    session.wait_for_upload_complete_or_client_stopped();

    // Nonempty
    write_transaction(db, [](WriteTransaction& wt) {
        wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
    });
    // Since the Barq is no longer empty, the following wait operation cannot
    // complete until the client has been in contact with the server, and caused
    // the server to create the server-side file
    session.wait_for_upload_complete_or_client_stopped();
    CHECK(util::File::exists(server_path));

    // Already done
    session.wait_for_upload_complete_or_client_stopped();

    // More changes
    write_transaction(db, [](WriteTransaction& wt) {
        wt.get_group().add_table_with_primary_key("class_bar", type_Int, "id");
    });
    session.wait_for_upload_complete_or_client_stopped();
}


TEST(Sync_WaitForUploadCompletionAfterEmptyTransaction)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db);
    ClientServerFixture fixture(dir, test_context);
    fixture.start();

    Session session = fixture.make_bound_session(db);
    for (int i = 0; i < 100; ++i) {
        WriteTransaction wt(db);
        wt.commit();
        session.wait_for_upload_complete_or_client_stopped();
    }
    {
        WriteTransaction wt(db);
        wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
        wt.commit();
        session.wait_for_upload_complete_or_client_stopped();
    }
}


TEST(Sync_WaitForDownloadCompletion)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);
    ClientServerFixture fixture(dir, test_context);
    fixture.start();

    // Noting to download
    Session session_1 = fixture.make_bound_session(db_1);
    session_1.wait_for_download_complete_or_client_stopped();

    // Again
    session_1.wait_for_download_complete_or_client_stopped();

    // Upload something via session 2
    Session session_2 = fixture.make_bound_session(db_2);
    write_transaction(db_2, [](WriteTransaction& wt) {
        wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
    });
    session_2.wait_for_upload_complete_or_client_stopped();

    // Wait for session 1 to download it
    session_1.wait_for_download_complete_or_client_stopped();
    {
        ReadTransaction rt_1(db_1);
        ReadTransaction rt_2(db_2);
        CHECK(compare_groups(rt_1, rt_2, *test_context.logger));
    }

    // Again
    session_1.wait_for_download_complete_or_client_stopped();

    // Wait for session 2 to download nothing
    session_2.wait_for_download_complete_or_client_stopped();

    // Upload something via session 1
    write_transaction(db_1, [](WriteTransaction& wt) {
        wt.get_group().add_table_with_primary_key("class_bar", type_Int, "id");
    });
    session_1.wait_for_upload_complete_or_client_stopped();

    // Wait for session 2 to download it
    session_2.wait_for_download_complete_or_client_stopped();
    {
        ReadTransaction rt_1(db_1);
        ReadTransaction rt_2(db_2);
        CHECK(compare_groups(rt_1, rt_2, *test_context.logger));
    }
}


TEST(Sync_WaitForDownloadCompletionAfterEmptyTransaction)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db);
    ClientServerFixture fixture(dir, test_context);

    {
        WriteTransaction wt(db);
        wt.commit();
    }
    fixture.start();
    for (int i = 0; i < 8; ++i) {
        Session session = fixture.make_bound_session(db, "/test");
        session.wait_for_download_complete_or_client_stopped();
        session.wait_for_download_complete_or_client_stopped();
        {
            WriteTransaction wt(db);
            wt.commit();
        }
        session.wait_for_download_complete_or_client_stopped();
        session.wait_for_download_complete_or_client_stopped();
    }
}


TEST(Sync_WaitForDownloadCompletionManyConcurrent)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db);
    ClientServerFixture fixture(dir, test_context);
    fixture.start();

    Session session = fixture.make_bound_session(db);
    constexpr int num_threads = 8;
    std::thread threads[num_threads];
    for (int i = 0; i < num_threads; ++i) {
        auto handler = [&] {
            session.wait_for_download_complete_or_client_stopped();
        };
        threads[i] = std::thread{handler};
    }
    for (int i = 0; i < num_threads; ++i)
        threads[i].join();
}


TEST(Sync_WaitForSessionTerminations)
{
    TEST_DIR(server_dir);
    TEST_CLIENT_DB(db);

    ClientServerFixture fixture(server_dir, test_context);
    fixture.start();

    Session session = fixture.make_bound_session(db, "/test");
    session.wait_for_download_complete_or_client_stopped();
    // Note: Atomicity would not be needed if
    // Session::async_wait_for_download_completion() was assumed to work.
    std::atomic<bool> called{false};
    auto handler = [&](Status) {
        called = true;
    };
    session.async_wait_for_download_completion(std::move(handler));
    session.detach();
    // The completion handler of an asynchronous wait operation is guaranteed
    // to be called, and no later than at session termination time. Also, any
    // callback function associated with a session on which termination has been
    // initiated, including the completion handler of the asynchronous wait
    // operation, must have finished executing when
    // Client::wait_for_session_terminations_or_client_stopped() returns.
    fixture.wait_for_session_terminations_or_client_stopped();
    CHECK(called);
}


TEST(Sync_TokenWithoutExpirationAllowed)
{
    bool did_fail = false;
    {
        TEST_DIR(dir);
        TEST_CLIENT_DB(db);
        ClientServerFixture fixture(dir, test_context);

        auto listener = [&](ConnectionState state, util::Optional<ErrorInfo> error_info) {
            if (state != ConnectionState::disconnected)
                return;
            BARQ_ASSERT(error_info);
            CHECK_EQUAL(error_info->status, ErrorCodes::SyncPermissionDenied);
            did_fail = true;
            fixture.stop();
        };

        fixture.start();

        Session::Config sess_config;
        sess_config.signed_user_token = g_signed_test_user_token_expiration_unspecified;
        sess_config.connection_state_change_listener = listener;
        Session session = fixture.make_session(db, "/test", std::move(sess_config));
        write_transaction(db, [](WriteTransaction& wt) {
            wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
        });
        session.wait_for_upload_complete_or_client_stopped();
        session.wait_for_download_complete_or_client_stopped();
    }
    CHECK_NOT(did_fail);
}


TEST(Sync_TokenWithNullExpirationAllowed)
{
    bool did_fail = false;
    {
        TEST_DIR(dir);
        TEST_CLIENT_DB(db);
        ClientServerFixture fixture(dir, test_context);
        auto error_handler = [&](Status, bool) {
            did_fail = true;
            fixture.stop();
        };
        fixture.set_client_side_error_handler(error_handler);
        fixture.start();

        Session::Config config;
        config.signed_user_token = g_signed_test_user_token_expiration_null;
        Session session = fixture.make_session(db, "/test", std::move(config));
        {
            write_transaction(db, [](WriteTransaction& wt) {
                wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
            });
        }
        session.wait_for_upload_complete_or_client_stopped();
        session.wait_for_download_complete_or_client_stopped();
    }
    CHECK_NOT(did_fail);
}


TEST(Sync_Upload)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db);
    ClientServerFixture fixture(dir, test_context);
    fixture.start();

    Session session = fixture.make_bound_session(db);

    {
        write_transaction(db, [](WriteTransaction& wt) {
            TableRef table = wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
            table->add_column(type_Int, "i");
        });
        for (int i = 0; i < 100; ++i) {
            WriteTransaction wt(db);
            TableRef table = wt.get_table("class_foo");
            table->create_object_with_primary_key(i);
            wt.commit();
        }
    }
    session.wait_for_upload_complete_or_client_stopped();
    session.wait_for_download_complete_or_client_stopped();
}


TEST(Sync_Replication)
{
    // Replicate changes in file 1 to file 2.

    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    {
        TEST_DIR(dir);
        ClientServerFixture fixture(dir, test_context);
        fixture.start();

        Session session_1 = fixture.make_bound_session(db_1);
        Session session_2 = fixture.make_session(db_2, "/test");

        // Create schema
        write_transaction(db_1, [](WriteTransaction& wt) {
            TableRef table = wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
            table->add_column(type_Int, "i");
        });
        Random random(random_int<unsigned long>()); // Seed from slow global generator
        for (int i = 0; i < 100; ++i) {
            WriteTransaction wt(db_1);
            TableRef table = wt.get_table("class_foo");
            table->create_object_with_primary_key(i);
            Obj obj = *(table->begin() + random.draw_int_mod(table->size()));
            obj.set<int64_t>("i", random.draw_int_max(0x7FFFFFFFFFFFFFFF));
            wt.commit();
        }

        session_1.wait_for_upload_complete_or_client_stopped();
        session_2.wait_for_download_complete_or_client_stopped();
    }

    ReadTransaction rt_1(db_1);
    ReadTransaction rt_2(db_2);
    const Group& group_1 = rt_1;
    const Group& group_2 = rt_2;
    group_1.verify();
    group_2.verify();
    CHECK(compare_groups(rt_1, rt_2, *test_context.logger));
    ConstTableRef table = group_1.get_table("class_foo");
    CHECK_EQUAL(100, table->size());
}


TEST(Sync_Merge)
{

    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    {
        TEST_DIR(dir);
        MultiClientServerFixture fixture(2, 1, dir, test_context);
        fixture.start();

        Session session_1 = fixture.make_session(0, 0, db_1, "/test");
        Session session_2 = fixture.make_session(1, 0, db_2, "/test");

        // Create schema on both clients.
        auto create_schema = [](DBRef db) {
            WriteTransaction wt(db);
            if (wt.has_table("class_foo"))
                return;
            TableRef table = wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
            table->add_column(type_Int, "i");
            wt.commit();
        };
        create_schema(db_1);
        create_schema(db_2);

        write_transaction(db_1, [](WriteTransaction& wt) {
            TableRef table = wt.get_table("class_foo");
            table->create_object_with_primary_key(1).set("i", 5);
            table->create_object_with_primary_key(2).set("i", 6);
        });
        write_transaction(db_2, [](WriteTransaction& wt) {
            TableRef table = wt.get_table("class_foo");
            table->create_object_with_primary_key(3).set("i", 7);
            table->create_object_with_primary_key(4).set("i", 8);
        });

        session_1.wait_for_upload_complete_or_client_stopped();
        session_2.wait_for_upload_complete_or_client_stopped();
        session_1.wait_for_download_complete_or_client_stopped();
        session_2.wait_for_download_complete_or_client_stopped();
    }

    ReadTransaction rt_1(db_1);
    ReadTransaction rt_2(db_2);
    const Group& group_1 = rt_1;
    const Group& group_2 = rt_2;
    group_1.verify();
    group_2.verify();
    CHECK(compare_groups(rt_1, rt_2, *test_context.logger));
    ConstTableRef table = group_1.get_table("class_foo");
    CHECK_EQUAL(4, table->size());
}

struct ExpectChangesetError {
    unit_test::TestContext& test_context;
    MultiClientServerFixture& fixture;
    std::string expected_error;

    void operator()(ConnectionState state, util::Optional<ErrorInfo> error_info) const noexcept
    {
        if (state == ConnectionState::disconnected) {
            return;
        }
        if (!error_info)
            return;
        BARQ_ASSERT(error_info);
        CHECK_EQUAL(error_info->status, ErrorCodes::BadChangeset);
        CHECK(!error_info->is_fatal);
        CHECK_EQUAL(error_info->status.reason(),
                    "Failed to transform received changeset: Schema mismatch: " + expected_error);
        fixture.stop();
    }
};

void test_schema_mismatch(unit_test::TestContext& test_context, util::FunctionRef<void(WriteTransaction&)> fn_1,
                          util::FunctionRef<void(WriteTransaction&)> fn_2, const char* expected_error_1,
                          const char* expected_error_2 = nullptr)
{
    auto perform_write_transaction = [](DBRef db, util::FunctionRef<void(WriteTransaction&)> function) {
        WriteTransaction wt(db);
        function(wt);
        return wt.commit();
    };

    TEST_DIR(dir);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    perform_write_transaction(db_1, fn_1);
    perform_write_transaction(db_2, fn_2);

    MultiClientServerFixture fixture(2, 1, dir, test_context);
    fixture.allow_server_errors(0, 1);
    fixture.start();

    if (!expected_error_2)
        expected_error_2 = expected_error_1;

    Session::Config config_1;
    config_1.connection_state_change_listener = ExpectChangesetError{test_context, fixture, expected_error_1};
    Session::Config config_2;
    config_2.connection_state_change_listener = ExpectChangesetError{test_context, fixture, expected_error_2};

    Session session_1 = fixture.make_session(0, 0, db_1, "/test", std::move(config_1));
    Session session_2 = fixture.make_session(1, 0, db_2, "/test", std::move(config_2));

    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_upload_complete_or_client_stopped();
    session_1.wait_for_download_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();
}


TEST(Sync_DetectSchemaMismatch_ColumnType)
{
    test_schema_mismatch(
        test_context,
        [](WriteTransaction& wt) {
            TableRef table = wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
            ColKey col_ndx = table->add_column(type_Int, "column");
            table->create_object_with_primary_key(1).set<int64_t>(col_ndx, 123);
        },
        [](WriteTransaction& wt) {
            TableRef table = wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
            ColKey col_ndx = table->add_column(type_String, "column");
            table->create_object_with_primary_key(2).set(col_ndx, "Hello, World!");
        },
        "Property 'column' in class 'foo' is of type Int on one side and type String on the other.",
        "Property 'column' in class 'foo' is of type String on one side and type Int on the other.");
}


TEST(Sync_DetectSchemaMismatch_Nullability)
{
    test_schema_mismatch(
        test_context,
        [](WriteTransaction& wt) {
            TableRef table = wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
            bool nullable = false;
            ColKey col_ndx = table->add_column(type_Int, "column", nullable);
            table->create_object_with_primary_key(1).set<int64_t>(col_ndx, 123);
        },
        [](WriteTransaction& wt) {
            TableRef table = wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
            bool nullable = true;
            ColKey col_ndx = table->add_column(type_Int, "column", nullable);
            table->create_object_with_primary_key(2).set<int64_t>(col_ndx, 123);
        },
        "Property 'column' in class 'foo' is nullable on one side and not on the other.");
}


TEST(Sync_DetectSchemaMismatch_Links)
{
    test_schema_mismatch(
        test_context,
        [](WriteTransaction& wt) {
            TableRef table = wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
            TableRef target = wt.get_group().add_table_with_primary_key("class_bar", type_Int, "id");
            table->add_column(*target, "column");
        },
        [](WriteTransaction& wt) {
            TableRef table = wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
            TableRef target = wt.get_group().add_table_with_primary_key("class_baz", type_Int, "id");
            table->add_column(*target, "column");
        },
        "Link property 'column' in class 'foo' points to class 'bar' on one side and to 'baz' on the other.",
        "Link property 'column' in class 'foo' points to class 'baz' on one side and to 'bar' on the other.");
}


TEST(Sync_DetectSchemaMismatch_PrimaryKeys_Name)
{
    test_schema_mismatch(
        test_context,
        [](WriteTransaction& wt) {
            wt.get_group().add_table_with_primary_key("class_foo", type_Int, "a");
        },
        [](WriteTransaction& wt) {
            wt.get_group().add_table_with_primary_key("class_foo", type_Int, "b");
        },
        "'foo' has primary key 'a' on one side, but primary key 'b' on the other.",
        "'foo' has primary key 'b' on one side, but primary key 'a' on the other.");
}


TEST(Sync_DetectSchemaMismatch_PrimaryKeys_Type)
{
    test_schema_mismatch(
        test_context,
        [](WriteTransaction& wt) {
            wt.get_group().add_table_with_primary_key("class_foo", type_Int, "a");
        },
        [](WriteTransaction& wt) {
            wt.get_group().add_table_with_primary_key("class_foo", type_String, "a");
        },
        "'foo' has primary key 'a', which is of type Int on one side and type String on the other.",
        "'foo' has primary key 'a', which is of type String on one side and type Int on the other.");
}


TEST(Sync_DetectSchemaMismatch_PrimaryKeys_Nullability)
{
    test_schema_mismatch(
        test_context,
        [](WriteTransaction& wt) {
            bool nullable = false;
            wt.get_group().add_table_with_primary_key("class_foo", type_Int, "a", nullable);
        },
        [](WriteTransaction& wt) {
            bool nullable = true;
            wt.get_group().add_table_with_primary_key("class_foo", type_Int, "a", nullable);
        },
        "'foo' has primary key 'a', which is nullable on one side, but not the other.");
}


TEST(Sync_LateBind)
{
    // Test that a session can be initiated at a point in time where the client
    // already has established a connection to the server.

    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    {
        TEST_DIR(dir);
        ClientServerFixture fixture(dir, test_context);
        fixture.start();

        Session session_1 = fixture.make_bound_session(db_1);
        write_transaction(db_1, [](WriteTransaction& wt) {
            wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
        });
        session_1.wait_for_upload_complete_or_client_stopped();

        Session session_2 = fixture.make_bound_session(db_2);
        write_transaction(db_2, [](WriteTransaction& wt) {
            wt.get_group().add_table_with_primary_key("class_bar", type_Int, "id");
        });
        session_2.wait_for_upload_complete_or_client_stopped();

        session_1.wait_for_download_complete_or_client_stopped();
        session_2.wait_for_download_complete_or_client_stopped();
    }

    ReadTransaction rt_1(db_1);
    ReadTransaction rt_2(db_2);
    const Group& group_1 = rt_1;
    const Group& group_2 = rt_2;
    group_1.verify();
    group_2.verify();
    CHECK(compare_groups(rt_1, rt_2, *test_context.logger));
    CHECK_EQUAL(2, group_1.size());
}


TEST(Sync_EarlyUnbind)
{
    // Verify that it is possible to unbind one session while another session
    // keeps the connection to the server open.

    TEST_DIR(dir);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);
    TEST_CLIENT_DB(db_3);
    ClientServerFixture fixture(dir, test_context);
    fixture.start();

    // Session 1 is here only to keep the connection alive
    Session session_1 = fixture.make_bound_session(db_1, "/dummy");
    {
        Session session_2 = fixture.make_bound_session(db_2);
        write_transaction(db_2, [](WriteTransaction& wt) {
            wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
        });
        session_2.wait_for_upload_complete_or_client_stopped();
        // Session 2 is now connected, but will be abandoned at end of scope
    }
    {
        // Starting a new session 3 forces closure of all previously abandoned
        // sessions, in turn forcing session 2 to be enlisted for writing its
        // UNBIND before session 3 is enlisted for writing BIND.
        Session session_3 = fixture.make_bound_session(db_3);
        // We now use MARK messages to wait for a complete unbind of session
        // 2. The client is guaranteed to receive the UNBIND response for session
        // 2 before it receives the MARK response for session 3.
        session_3.wait_for_download_complete_or_client_stopped();
    }
}


TEST(Sync_FastRebind)
{
    // Verify that it is possible to create multiple immediately consecutive
    // sessions for the same Barq file.

    TEST_DIR(dir);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);
    ClientServerFixture fixture(dir, test_context);
    fixture.start();

    // Session 1 is here only to keep the connection alive
    Session session_1 = fixture.make_bound_session(db_1, "/dummy");
    {
        Session session_2 = fixture.make_bound_session(db_2, "/test");
        WriteTransaction wt(db_2);
        TableRef table = wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
        table->add_column(type_Int, "i");
        table->create_object_with_primary_key(1);
        wt.commit();
        session_2.wait_for_upload_complete_or_client_stopped();
    }
    for (int i = 0; i < 100; ++i) {
        Session session_2 = fixture.make_bound_session(db_2, "/test");
        WriteTransaction wt(db_2);
        TableRef table = wt.get_table("class_foo");
        table->begin()->set<int64_t>("i", i);
        wt.commit();
        session_2.wait_for_upload_complete_or_client_stopped();
    }
}


TEST(Sync_UnbindBeforeActivation)
{
    // This test tries to make it likely that the server receives an UNBIND
    // message for a session that is still not activated, i.e., before the
    // server receives the IDENT message.

    TEST_DIR(dir);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);
    ClientServerFixture fixture(dir, test_context);
    fixture.start();

    // Session 1 is here only to keep the connection alive
    Session session_1 = fixture.make_bound_session(db_1);
    for (int i = 0; i < 1000; ++i) {
        Session session_2 = fixture.make_bound_session(db_2);
        session_2.wait_for_upload_complete_or_client_stopped();
    }
}


#if 0  // FIXME: Disabled because substring operations are not yet supported in Core 6.

// This test illustrates that our instruction set and merge rules
// do not have higher order convergence. The final merge result depends
// on the order with which the changesets reach the server. This example
// employs three clients operating on the same state. The state consists
// of two tables, "source" and "target". "source" has a link list pointing
// to target. Target contains three rows 0, 1, and 2. Source contains one
// row with a link list whose value is 2.
//
// The three clients produce changesets with client 1 having the earliest time
// stamp, client 2 the middle time stamp, and client 3 the latest time stamp.
// The clients produce the following changesets.
//
// client 1: target.move_last_over(0)
// client 2: source.link_list.set(0, 0);
// client 3: source.link_list.set(0, 1);
//
// In part a of the test, the order of the clients reaching the server is
// 1, 2, 3. The result is an empty link list since the merge of client 1 and 2
// produces a nullify link list instruction.
//
// In part b, the order of the clients reaching the server is 3, 1, 2. The
// result is a link list of size 1, since client 3 wins due to having the
// latest time stamp.
//
// If the "natural" peer to peer system of these merge rules were built, the
// transition from server a to server b involves an insert link instruction. In
// other words, the diff between two servers differing in the order of one
// move_last_over and two link_list_set instructions is an insert instruction.
// Repeated application of the pairwise merge rules would never produce this
// result.
//
// The test is not run in general since it just checks that we do not have
// higher order convergence, and the absence of higher order convergence is not
// a desired feature in itself.
TEST_IF(Sync_NonDeterministicMerge, false)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db_a1);
    TEST_CLIENT_DB(db_a2);
    TEST_CLIENT_DB(db_a3);
    TEST_CLIENT_DB(db_b1);
    TEST_CLIENT_DB(db_b2);
    TEST_CLIENT_DB(db_b3);

    ClientServerFixture fixture{dir, test_context};
    fixture.start();

    // Part a of the test.
    {
        WriteTransaction wt{db_a1};

        TableRef table_target = wt.get_group().add_table_with_primary_key("class_target");
        ColKey col_ndx = table_target->add_column(type_Int, "value");
        CHECK_EQUAL(col_ndx, 1);
        Obj row0 = table_target->create_object_with_primary_key(i);
        Obj row1 = table_target->create_object_with_primary_key(i);
        Obj row2 = table_target->create_object_with_primary_key(i);
        row0.set(col_ndx, 123);
        row1.set(col_ndx, 456);
        row2.set(col_ndx, 789);

        TableRef table_source = wt.get_group().add_table_with_primary_key("class_source");
        col_ndx = table_source->add_column_link(type_LinkList, "target_link",
                                                *table_target);
        CHECK_EQUAL(col_ndx, 1);
        Obj obj = table_source->create_object_with_primary_key(i);
        auto ll = obj.get_linklist(col_ndx);
        ll.insert(0, row2.get_key());
        CHECK_EQUAL(ll.size(), 1);
        wt.commit();
    }

    {
        Session session = fixture.make_bound_session(db_a1, "/server-path-a");
        session.wait_for_upload_complete_or_client_stopped();
    }

    {
        Session session = fixture.make_bound_session(db_a2, "/server-path-a");
        session.wait_for_download_complete_or_client_stopped();
    }

    {
        Session session = fixture.make_bound_session(db_a3, "/server-path-a");
        session.wait_for_download_complete_or_client_stopped();
    }

    {
        WriteTransaction wt{db_a1};
        TableRef table = wt.get_table("class_target");
        table->remove_object(table->begin());
        CHECK_EQUAL(table->size(), 2);
        wt.commit();
    }

    {
        WriteTransaction wt{db_a2};
        TableRef table = wt.get_table("class_source");
        auto ll = table->get_linklist(1, 0);
        CHECK_EQUAL(ll->size(), 1);
        CHECK_EQUAL(ll->get(0).get_int(1), 789);
        ll->set(0, 0);
        CHECK_EQUAL(ll->get(0).get_int(1), 123);
        wt.commit();
    }

    {
        WriteTransaction wt{db_a3};
        TableRef table = wt.get_table("class_source");
        auto ll = table->get_linklist(1, 0);
        CHECK_EQUAL(ll->size(), 1);
        CHECK_EQUAL(ll->get(0).get_int(1), 789);
        ll->set(0, 1);
        CHECK_EQUAL(ll->get(0).get_int(1), 456);
        wt.commit();
    }

    {
        Session session = fixture.make_bound_session(db_a1, "/server-path-a");
        session.wait_for_upload_complete_or_client_stopped();
    }

    {
        Session session = fixture.make_bound_session(db_a2, "/server-path-a");
        session.wait_for_upload_complete_or_client_stopped();
    }

    {
        Session session = fixture.make_bound_session(db_a3, "/server-path-a");
        session.wait_for_upload_complete_or_client_stopped();
    }

    {
        Session session = fixture.make_bound_session(db_a1, "/server-path-a");
        session.wait_for_download_complete_or_client_stopped();
    }

    // Part b of the test.
    {
        WriteTransaction wt{db_b1};

        TableRef table_target = wt.get_group().add_table_with_primary_key("class_target");
        ColKey col_ndx = table_target->add_column(type_Int, "value");
        CHECK_EQUAL(col_ndx, 1);
        table_target->create_object_with_primary_key(i);
        table_target->create_object_with_primary_key(i);
        table_target->create_object_with_primary_key(i);
        table_target->begin()->set(col_ndx, 123);
        table_target->get_object(1).set(col_ndx, 456);
        table_target->get_object(2).set(col_ndx, 789);

        TableRef table_source = wt.get_group().add_table_with_primary_key("class_source");
        col_ndx = table_source->add_column_link(type_LinkList, "target_link",
                                                *table_target);
        CHECK_EQUAL(col_ndx, 1);
        table_source->create_object_with_primary_key(i);
        auto ll = table_source->get_linklist(col_ndx, 0);
        ll->insert(0, 2);
        CHECK_EQUAL(ll->size(), 1);
        wt.commit();
    }

    {
        Session session = fixture.make_bound_session(db_b1, "/server-path-b");
        session.wait_for_upload_complete_or_client_stopped();
    }

    {
        Session session = fixture.make_bound_session(db_b2, "/server-path-b");
        session.wait_for_download_complete_or_client_stopped();
    }

    {
        Session session = fixture.make_bound_session(db_b3, "/server-path-b");
        session.wait_for_download_complete_or_client_stopped();
    }

    {
        WriteTransaction wt{db_b1};
        TableRef table = wt.get_table("class_target");
        table->move_last_over(0);
        CHECK_EQUAL(table->size(), 2);
        wt.commit();
    }

    {
        WriteTransaction wt{db_b2};
        TableRef table = wt.get_table("class_source");
        auto ll = table->get_linklist(1, 0);
        CHECK_EQUAL(ll->size(), 1);
        CHECK_EQUAL(ll->get(0).get_int(1), 789);
        ll->set(0, 0);
        CHECK_EQUAL(ll->get(0).get_int(1), 123);
        wt.commit();
    }

    {
        WriteTransaction wt{db_b3};
        TableRef table = wt.get_table("class_source");
        auto ll = table->get_linklist(1, 0);
        CHECK_EQUAL(ll->size(), 1);
        CHECK_EQUAL(ll->get(0).get_int(1), 789);
        ll->set(0, 1);
        CHECK_EQUAL(ll->get(0).get_int(1), 456);
        wt.commit();
    }

    // The crucial difference between part a and b is that client 3
    // uploads it changes first in part b and last in part a.
    {
        Session session = fixture.make_bound_session(db_b3, "/server-path-b");
        session.wait_for_upload_complete_or_client_stopped();
    }

    {
        Session session = fixture.make_bound_session(db_b1, "/server-path-b");
        session.wait_for_upload_complete_or_client_stopped();
    }

    {
        Session session = fixture.make_bound_session(db_b2, "/server-path-b");
        session.wait_for_upload_complete_or_client_stopped();
    }

    {
        Session session = fixture.make_bound_session(db_b1, "/server-path-b");
        session.wait_for_download_complete_or_client_stopped();
    }


    // Check the end result.

    size_t size_link_list_a;
    size_t size_link_list_b;

    {
        ReadTransaction wt{db_a1};
        ConstTableRef table = wt.get_table("class_source");
        auto ll = table->get_linklist(1, 0);
        size_link_list_a = ll->size();
    }

    {
        ReadTransaction wt{db_b1};
        ConstTableRef table = wt.get_table("class_source");
        auto ll = table->get_linklist(1, 0);
        size_link_list_b = ll->size();
        CHECK_EQUAL(ll->size(), 1);
    }

    // The final link list has size 0 in part a and size 1 in part b.
    // These checks confirm that the OT system behaves as expected.
    // The expected behavior is higher order divergence.
    CHECK_EQUAL(size_link_list_a, 0);
    CHECK_EQUAL(size_link_list_b, 1);
    CHECK_NOT_EQUAL(size_link_list_a, size_link_list_b);
}
#endif // 0


TEST(Sync_Randomized)
{
    constexpr size_t num_clients = 7;

    auto client_test_program = [](DBRef db) {
        // Create the schema
        write_transaction(db, [](WriteTransaction& wt) {
            if (wt.has_table("class_foo"))
                return;
            TableRef table = wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
            table->add_column(type_Int, "i");
            table->create_object_with_primary_key(1);
        });

        Random random(random_int<unsigned long>()); // Seed from slow global generator
        for (int i = 0; i < 100; ++i) {
            WriteTransaction wt(db);
            if (random.chance(4, 5)) {
                TableRef table = wt.get_table("class_foo");
                if (random.chance(1, 5)) {
                    table->create_object_with_primary_key(i);
                }
                int value = random.draw_int(-32767, 32767);
                size_t row_ndx = random.draw_int_mod(table->size());
                table->get_object(row_ndx).set("i", value);
            }
            wt.commit();
        }
    };

    TEST_DIR(dir);
    MultiClientServerFixture fixture(num_clients, 1, dir, test_context);
    fixture.start();

    std::unique_ptr<DBTestPathGuard> client_path_guards[num_clients];
    DBRef client_shared_groups[num_clients];
    for (size_t i = 0; i < num_clients; ++i) {
        std::string suffix = util::format(".client_%1.barq", i);
        std::string test_path = get_test_path(test_context.get_test_name(), suffix);
        client_path_guards[i].reset(new DBTestPathGuard(test_path));
        client_shared_groups[i] = DB::create(make_client_replication(), test_path);
    }

    std::vector<Session> sessions(num_clients);
    for (size_t i = 0; i < num_clients; ++i) {
        auto db = client_shared_groups[i];
        sessions[i] = fixture.make_session(int(i), 0, db, "/test");
    }

    auto run_client_test_program = [&](size_t i) {
        try {
            client_test_program(client_shared_groups[i]);
        }
        catch (...) {
            fixture.stop();
            throw;
        }
    };

    ThreadWrapper client_program_threads[num_clients];
    for (size_t i = 0; i < num_clients; ++i)
        client_program_threads[i].start([=] {
            run_client_test_program(i);
        });

    for (size_t i = 0; i < num_clients; ++i)
        CHECK(!client_program_threads[i].join());

    log("All client programs completed");

    // Wait until all local changes are uploaded, and acknowledged by the
    // server.
    for (size_t i = 0; i < num_clients; ++i)
        sessions[i].wait_for_upload_complete_or_client_stopped();

    log("Everything uploaded");

    // Now wait for all previously uploaded changes to be downloaded by all
    // others.
    for (size_t i = 0; i < num_clients; ++i)
        sessions[i].wait_for_download_complete_or_client_stopped();

    log("Everything downloaded");

    BARQ_ASSERT(num_clients > 0);
    ReadTransaction rt_0(client_shared_groups[0]);
    rt_0.get_group().verify();
    for (size_t i = 1; i < num_clients; ++i) {
        ReadTransaction rt(client_shared_groups[i]);
        rt.get_group().verify();
        // Logger is guaranteed to be defined
        CHECK(compare_groups(rt_0, rt, *test_context.logger));
    }
}

#ifdef BARQ_DEBUG // Failure simulation only works in debug mode

TEST(Sync_ReadFailureSimulation)
{
    TEST_DIR(server_dir);
    TEST_CLIENT_DB(db);

    // Check that read failure simulation works on the client-side
    {
        bool client_side_read_did_fail = false;
        {
            ClientServerFixture fixture(server_dir, test_context);
            fixture.set_client_side_error_rate(1, 1); // 100% chance of failure
            auto error_handler = [&](Status status, bool is_fatal) {
                CHECK_EQUAL(status, ErrorCodes::RuntimeError);
                CHECK_EQUAL(status.reason(), "Simulated failure during sync client websocket read");
                CHECK_NOT(is_fatal);
                client_side_read_did_fail = true;
                fixture.stop();
            };
            fixture.set_client_side_error_handler(error_handler);
            Session session = fixture.make_bound_session(db, "/test");
            fixture.start();
            session.wait_for_download_complete_or_client_stopped();
        }
        CHECK(client_side_read_did_fail);
    }

    // FIXME: Figure out a way to check that read failure simulation works on
    // the server-side
}

#endif // BARQ_DEBUG
TEST(Sync_FailingReadsOnClientSide)
{
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    {
        TEST_DIR(dir);
        ClientServerFixture fixture{dir, test_context};
        fixture.set_client_side_error_rate(5, 100); // 5% chance of failure
        auto error_handler = [&](Status status, bool is_fatal) {
            if (CHECK_EQUAL(status.reason(), "Simulated failure during sync client websocket read")) {
                CHECK_EQUAL(status, ErrorCodes::RuntimeError);
                CHECK_NOT(is_fatal);
                fixture.cancel_reconnect_delay();
            }
        };
        fixture.set_client_side_error_handler(error_handler);
        fixture.start();

        Session session_1 = fixture.make_bound_session(db_1);

        Session session_2 = fixture.make_bound_session(db_2);

        write_transaction(db_1, [](WriteTransaction& wt) {
            TableRef table = wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
            table->add_column(type_Int, "i");
            table->create_object_with_primary_key(1);
        });
        write_transaction(db_2, [](WriteTransaction& wt) {
            TableRef table = wt.get_group().add_table_with_primary_key("class_bar", type_Int, "id");
            table->add_column(type_Int, "i");
            table->create_object_with_primary_key(2);
        });
        for (int i = 0; i < 100; ++i) {
            session_1.wait_for_upload_complete_or_client_stopped();
            session_2.wait_for_upload_complete_or_client_stopped();
            for (int i = 0; i < 10; ++i) {
                write_transaction(db_1, [=](WriteTransaction& wt) {
                    TableRef table = wt.get_table("class_foo");
                    table->begin()->set("i", i);
                });
                write_transaction(db_2, [=](WriteTransaction& wt) {
                    TableRef table = wt.get_table("class_bar");
                    table->begin()->set("i", i);
                });
            }
        }
        session_1.wait_for_upload_complete_or_client_stopped();
        session_2.wait_for_upload_complete_or_client_stopped();
        session_1.wait_for_download_complete_or_client_stopped();
        session_2.wait_for_download_complete_or_client_stopped();
    }

    ReadTransaction rt_1(db_1);
    ReadTransaction rt_2(db_2);
    const Group& group_1 = rt_1;
    const Group& group_2 = rt_2;
    group_1.verify();
    group_2.verify();
    CHECK(compare_groups(rt_1, rt_2, *test_context.logger));
}


TEST(Sync_FailingReadsOnServerSide)
{
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    {
        TEST_DIR(dir);
        ClientServerFixture fixture{dir, test_context};
        fixture.set_server_side_error_rate(5, 100); // 5% chance of failure
        auto error_handler = [&](Status, bool is_fatal) {
            CHECK_NOT(is_fatal);
            fixture.cancel_reconnect_delay();
        };
        fixture.set_client_side_error_handler(error_handler);
        fixture.start();

        Session session_1 = fixture.make_bound_session(db_1);

        Session session_2 = fixture.make_bound_session(db_2);

        write_transaction(db_1, [](WriteTransaction& wt) {
            TableRef table = wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
            table->add_column(type_Int, "i");
            table->create_object_with_primary_key(1);
        });
        write_transaction(db_2, [](WriteTransaction& wt) {
            TableRef table = wt.get_group().add_table_with_primary_key("class_bar", type_Int, "id");
            table->add_column(type_Int, "i");
            table->create_object_with_primary_key(2);
        });
        for (int i = 0; i < 100; ++i) {
            session_1.wait_for_upload_complete_or_client_stopped();
            session_2.wait_for_upload_complete_or_client_stopped();
            for (int i = 0; i < 10; ++i) {
                write_transaction(db_1, [=](WriteTransaction& wt) {
                    TableRef table = wt.get_table("class_foo");
                    table->begin()->set("i", i);
                });
                write_transaction(db_2, [=](WriteTransaction& wt) {
                    TableRef table = wt.get_table("class_bar");
                    table->begin()->set("i", i);
                });
            }
        }
        session_1.wait_for_upload_complete_or_client_stopped();
        session_2.wait_for_upload_complete_or_client_stopped();
        session_1.wait_for_download_complete_or_client_stopped();
        session_2.wait_for_download_complete_or_client_stopped();
    }

    ReadTransaction rt_1(db_1);
    ReadTransaction rt_2(db_2);
    const Group& group_1 = rt_1;
    const Group& group_2 = rt_2;
    group_1.verify();
    group_2.verify();
    CHECK(compare_groups(rt_1, rt_2, *test_context.logger));
}


TEST(Sync_ErrorAfterServerRestore_BadClientFileIdent)
{
    TEST_DIR(server_dir);
    TEST_CLIENT_DB(db);

    std::string server_path = "/test";
    std::string server_barq_path;

    // Make a change and synchronize with server
    {
        ClientServerFixture fixture(server_dir, test_context);
        server_barq_path = fixture.map_virtual_to_real_path(server_path);
        Session session = fixture.make_bound_session(db, server_path);
        WriteTransaction wt{db};
        wt.get_group().add_table_with_primary_key("class_table", type_Int, "id");
        wt.commit();
        fixture.start();
        session.wait_for_upload_complete_or_client_stopped();
    }

    // Emulate a server-side restore to before the creation of the Barq
    util::File::remove(server_barq_path);

    // Provoke error by attempting to resynchronize
    bool did_fail = false;
    {
        ClientServerFixture fixture(server_dir, test_context);
        auto error_handler = [&](Status status, bool is_fatal) {
            CHECK_EQUAL(status, ErrorCodes::SyncClientResetRequired);
            CHECK(is_fatal);
            did_fail = true;
            fixture.stop();
        };
        fixture.set_client_side_error_handler(error_handler);
        Session session = fixture.make_bound_session(db, server_path);
        fixture.start();
        session.wait_for_download_complete_or_client_stopped();
    }
    CHECK(did_fail);
}


TEST(Sync_HTTP404NotFound)
{
    TEST_DIR(server_dir);

    std::string server_address = "localhost";

    Server::Config server_config;
    server_config.logger = std::make_shared<util::PrefixLogger>("Server: ", test_context.logger);
    server_config.listen_address = server_address;
    server_config.listen_port = "";
    server_config.tcp_no_delay = true;

    util::Optional<PKey> public_key = PKey::load_public(test_server_key_path());
    Server server(server_dir, std::move(public_key), server_config);
    server.start();
    network::Endpoint endpoint = server.listen_endpoint();

    ThreadWrapper server_thread;
    server_thread.start([&] {
        server.run();
    });

    HTTPRequest request;
    request.path = "/not-found";

    HTTPRequestClient client(test_context.logger, endpoint, request);
    client.fetch_response();

    server.stop();

    server_thread.join();

    const HTTPResponse& response = client.get_response();

    CHECK(response.status == HTTPStatus::NotFound);
    CHECK(response.headers.find("Server")->second == "BarqSync/" BARQ_VERSION_STRING);
}


namespace {

class RequestWithContentLength {
public:
    RequestWithContentLength(test_util::unit_test::TestContext& test_context, network::Service& service,
                             const network::Endpoint& endpoint, const std::string& content_length,
                             const std::string& expected_response_line)
        : test_context{test_context}
        , m_socket{service}
        , m_endpoint{endpoint}
        , m_content_length{content_length}
        , m_expected_response_line{expected_response_line}
    {
        m_request = "POST /does-not-exist-1234 HTTP/1.1\r\n"
                    "Content-Length: " +
                    m_content_length +
                    "\r\n"
                    "\r\n";
    }

    void write_completion_handler(std::error_code ec, size_t nbytes)
    {
        CHECK_NOT(ec);
        CHECK_EQUAL(m_request.size(), nbytes);
        auto handler = [&](std::error_code ec, size_t nbytes) {
            this->read_completion_handler(ec, nbytes);
        };
        m_socket.async_read_until(m_buffer, m_buf_size, '\n', m_read_ahead_buffer, handler);
    }

    void read_completion_handler(std::error_code ec, size_t nbytes)
    {
        CHECK_NOT(ec);
        std::string response_line{m_buffer, nbytes};
        CHECK_EQUAL(response_line, m_expected_response_line);
    }

    void start()
    {
        std::error_code ec;
        m_socket.connect(m_endpoint, ec);
        CHECK_NOT(ec);

        auto handler = [&](std::error_code ec, size_t nbytes) {
            this->write_completion_handler(ec, nbytes);
        };
        m_socket.async_write(m_request.data(), m_request.size(), handler);
    }

private:
    test_util::unit_test::TestContext& test_context;
    network::Socket m_socket;
    network::ReadAheadBuffer m_read_ahead_buffer;
    static constexpr size_t m_buf_size = 1000;
    char m_buffer[m_buf_size];
    const network::Endpoint& m_endpoint;
    const std::string m_content_length;
    std::string m_request;
    const std::string m_expected_response_line;
};

} // namespace

// Test the server's HTTP response to a Content-Length header of zero, empty,
// and a non-number string.
TEST(Sync_HTTP_ContentLength)
{
    TEST_DIR(server_dir);

    std::string server_address = "localhost";

    Server::Config server_config;
    server_config.logger = std::make_shared<util::PrefixLogger>("Server: ", test_context.logger);
    server_config.listen_address = server_address;
    server_config.listen_port = "";
    server_config.tcp_no_delay = true;

    util::Optional<PKey> public_key = PKey::load_public(test_server_key_path());
    Server server(server_dir, std::move(public_key), server_config);
    server.start();
    network::Endpoint endpoint = server.listen_endpoint();

    ThreadWrapper server_thread;
    server_thread.start([&] {
        server.run();
    });

    network::Service service;

    RequestWithContentLength req_0(test_context, service, endpoint, "0", "HTTP/1.1 404 Not Found\r\n");

    RequestWithContentLength req_1(test_context, service, endpoint, "", "HTTP/1.1 404 Not Found\r\n");

    RequestWithContentLength req_2(test_context, service, endpoint, "abc", "HTTP/1.1 400 Bad Request\r\n");

    RequestWithContentLength req_3(test_context, service, endpoint, "5abc", "HTTP/1.1 400 Bad Request\r\n");

    req_0.start();
    req_1.start();
    req_2.start();
    req_3.start();

    service.run();

    server.stop();
    server_thread.join();
}


TEST(Sync_ErrorAfterServerRestore_BadServerVersion)
{
    TEST_DIR(server_dir);
    TEST_DIR(backup_dir);
    TEST_CLIENT_DB(db);

    std::string server_path = "/test";
    std::string server_barq_path;
    std::string backup_barq_path = util::File::resolve("test.barq", backup_dir);

    // Create schema and synchronize with server
    {
        ClientServerFixture fixture(server_dir, test_context);
        server_barq_path = fixture.map_virtual_to_real_path(server_path);
        Session session = fixture.make_bound_session(db, server_path);
        WriteTransaction wt{db};
        TableRef table = wt.get_group().add_table_with_primary_key("class_table", type_Int, "id");
        table->add_column(type_Int, "column");
        wt.commit();
        fixture.start();
        session.wait_for_upload_complete_or_client_stopped();
    }

    // Save a snapshot of the server-side Barq file
    util::File::copy(server_barq_path, backup_barq_path);

    // Make change in which will be lost when restoring snapshot
    {
        ClientServerFixture fixture(server_dir, test_context);
        Session session = fixture.make_bound_session(db, server_path);
        WriteTransaction wt{db};
        TableRef table = wt.get_table("class_table");
        table->create_object_with_primary_key(1);
        wt.commit();
        fixture.start();
        session.wait_for_upload_complete_or_client_stopped();
    }

    // Restore the snapshot
    util::File::copy(backup_barq_path, server_barq_path);

    // Provoke error by resynchronizing
    bool did_fail = false;
    {
        ClientServerFixture fixture(server_dir, test_context);
        auto error_handler = [&](Status status, bool is_fatal) {
            CHECK_EQUAL(status, ErrorCodes::SyncClientResetRequired);
            CHECK(is_fatal);
            did_fail = true;
            fixture.stop();
        };
        fixture.set_client_side_error_handler(error_handler);
        Session session = fixture.make_bound_session(db, server_path);
        fixture.start();
        session.wait_for_download_complete_or_client_stopped();
    }
    CHECK(did_fail);
}


TEST(Sync_ErrorAfterServerRestore_BadClientVersion)
{
    TEST_DIR(server_dir);
    TEST_DIR(backup_dir);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    std::string server_path = "/test";
    std::string server_barq_path;
    std::string backup_barq_path = util::File::resolve("test.barq", backup_dir);

    // Create schema and synchronize client files
    {
        ClientServerFixture fixture(server_dir, test_context);
        server_barq_path = fixture.map_virtual_to_real_path(server_path);
        Session session_1 = fixture.make_bound_session(db_1, server_path);
        Session session_2 = fixture.make_bound_session(db_2, server_path);
        WriteTransaction wt{db_1};
        TableRef table = wt.get_group().add_table_with_primary_key("class_table", type_Int, "id");
        table->add_column(type_Int, "column");
        wt.commit();
        fixture.start();
        session_1.wait_for_upload_complete_or_client_stopped();
        session_2.wait_for_download_complete_or_client_stopped();
    }

    // Save a snapshot of the server-side Barq file
    util::File::copy(server_barq_path, backup_barq_path);

    // Make change in 1st file which will be lost when restoring snapshot
    {
        ClientServerFixture fixture(server_dir, test_context);
        Session session = fixture.make_bound_session(db_1, server_path);
        WriteTransaction wt{db_1};
        TableRef table = wt.get_table("class_table");
        table->create_object_with_primary_key(1);
        wt.commit();
        fixture.start();
        session.wait_for_upload_complete_or_client_stopped();
    }

    // Restore the snapshot
    util::File::copy(backup_barq_path, server_barq_path);

    // Make a conflicting change in 2nd file relative to reverted server state
    {
        ClientServerFixture fixture(server_dir, test_context);
        Session session = fixture.make_bound_session(db_2, server_path);
        WriteTransaction wt{db_2};
        TableRef table = wt.get_table("class_table");
        table->create_object_with_primary_key(2);
        wt.commit();
        fixture.start();
        session.wait_for_upload_complete_or_client_stopped();
    }

    // Provoke error by synchronizing 1st file
    bool did_fail = false;
    {
        ClientServerFixture fixture(server_dir, test_context);
        auto error_handler = [&](Status status, bool is_fatal) {
            CHECK_EQUAL(status, ErrorCodes::SyncClientResetRequired);
            CHECK(is_fatal);
            did_fail = true;
            fixture.stop();
        };
        fixture.set_client_side_error_handler(error_handler);
        Session session = fixture.make_bound_session(db_1, server_path);
        fixture.start();
        session.wait_for_download_complete_or_client_stopped();
    }
    CHECK(did_fail);
}


TEST(Sync_ErrorAfterServerRestore_BadClientFileIdentSalt)
{
    TEST_DIR(server_dir);
    TEST_DIR(backup_dir);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);
    TEST_CLIENT_DB(db_3);

    std::string server_path = "/test";
    std::string server_barq_path;
    std::string backup_barq_path = util::File::resolve("test.barq", backup_dir);

    // Register 1st file with server
    {
        ClientServerFixture fixture(server_dir, test_context);
        server_barq_path = fixture.map_virtual_to_real_path(server_path);
        Session session = fixture.make_bound_session(db_1, server_path);
        WriteTransaction wt{db_1};
        TableRef table = wt.get_group().add_table_with_primary_key("class_table_1", type_Int, "id");
        table->add_column(type_Int, "column");
        wt.commit();
        fixture.start();
        session.wait_for_upload_complete_or_client_stopped();
    }

    // Save a snapshot of the server-side Barq file
    util::File::copy(server_barq_path, backup_barq_path);

    // Register 2nd file with server
    {
        ClientServerFixture fixture(server_dir, test_context);
        Session session = fixture.make_bound_session(db_2, server_path);
        fixture.start();
        session.wait_for_download_complete_or_client_stopped();
    }

    // Restore the snapshot
    util::File::copy(backup_barq_path, server_barq_path);

    // Register 3rd conflicting file with server
    {
        ClientServerFixture fixture(server_dir, test_context);
        Session session = fixture.make_bound_session(db_3, server_path);
        fixture.start();
        session.wait_for_download_complete_or_client_stopped();
    }

    // Provoke error by resynchronizing 2nd file
    bool did_fail = false;
    {
        ClientServerFixture fixture(server_dir, test_context);
        auto error_handler = [&](Status status, bool is_fatal) {
            CHECK_EQUAL(status, ErrorCodes::SyncClientResetRequired);
            CHECK(is_fatal);
            did_fail = true;
            fixture.stop();
        };
        fixture.set_client_side_error_handler(error_handler);
        Session session = fixture.make_bound_session(db_2, server_path);
        fixture.start();
        session.wait_for_download_complete_or_client_stopped();
    }
    CHECK(did_fail);
}


TEST(Sync_ErrorAfterServerRestore_BadServerVersionSalt)
{
    TEST_DIR(server_dir);
    TEST_DIR(backup_dir);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);
    TEST_CLIENT_DB(db_3);

    std::string server_path = "/test";
    std::string server_barq_path;
    std::string backup_barq_path = util::File::resolve("test.barq", backup_dir);

    // Create schema and synchronize client files
    {
        ClientServerFixture fixture(server_dir, test_context);
        server_barq_path = fixture.map_virtual_to_real_path(server_path);
        Session session_1 = fixture.make_bound_session(db_1, server_path);
        Session session_2 = fixture.make_bound_session(db_2, server_path);
        Session session_3 = fixture.make_bound_session(db_3, server_path);
        WriteTransaction wt{db_1};
        TableRef table = wt.get_group().add_table_with_primary_key("class_table", type_Int, "id");
        table->add_column(type_Int, "column");
        wt.commit();
        fixture.start();
        session_1.wait_for_upload_complete_or_client_stopped();
        session_2.wait_for_download_complete_or_client_stopped();
        session_3.wait_for_download_complete_or_client_stopped();
    }

    // Save a snapshot of the server-side Barq file
    util::File::copy(server_barq_path, backup_barq_path);

    // Make change in 1st file which will be lost when restoring snapshot, and
    // make 2nd file download it.
    {
        ClientServerFixture fixture(server_dir, test_context);
        Session session_1 = fixture.make_bound_session(db_1, server_path);
        Session session_2 = fixture.make_bound_session(db_2, server_path);
        WriteTransaction wt{db_1};
        TableRef table = wt.get_table("class_table");
        table->create_object_with_primary_key(1);
        wt.commit();
        fixture.start();
        session_1.wait_for_upload_complete_or_client_stopped();
        session_2.wait_for_download_complete_or_client_stopped();
    }

    // Restore the snapshot
    util::File::copy(backup_barq_path, server_barq_path);

    // Make a conflicting change in 3rd file relative to reverted server state
    {
        ClientServerFixture fixture(server_dir, test_context);
        Session session = fixture.make_bound_session(db_3, server_path);
        WriteTransaction wt{db_3};
        TableRef table = wt.get_table("class_table");
        table->create_object_with_primary_key(2);
        wt.commit();
        fixture.start();
        session.wait_for_upload_complete_or_client_stopped();
    }

    // Provoke error by synchronizing 2nd file
    bool did_fail = false;
    {
        ClientServerFixture fixture(server_dir, test_context);
        auto error_handler = [&](Status status, bool is_fatal) {
            CHECK_EQUAL(status, ErrorCodes::SyncClientResetRequired);
            CHECK(is_fatal);
            did_fail = true;
            fixture.stop();
        };
        fixture.set_client_side_error_handler(error_handler);
        Session session = fixture.make_bound_session(db_2, server_path);
        fixture.start();
        session.wait_for_download_complete_or_client_stopped();
    }
    CHECK(did_fail);
}


TEST(Sync_MultipleServers)
{
    // Check that a client can make lots of connection to lots of servers in a
    // concurrent manner.

    const int num_servers = 2;
    const int num_barqs_per_server = 2;
    const int num_files_per_barq = 4;
    const int num_sessions_per_file = 8;
    const int num_transacts_per_session = 2;

    TEST_DIR(dir);
    int num_clients = 1;
    MultiClientServerFixture fixture(num_clients, num_servers, dir, test_context);
    fixture.start();

    TEST_DIR(dir_2);
    auto get_file_path = [&](int server_index, int barq_index, int file_index) {
        std::ostringstream out;
        out << server_index << "_" << barq_index << "_" << file_index << ".barq";
        return util::File::resolve(out.str(), dir_2);
    };
    std::atomic<int> id = 0;

    auto run = [&](int server_index, int barq_index, int file_index) {
        try {
            std::string path = get_file_path(server_index, barq_index, file_index);
            DBRef db = DB::create(make_client_replication(), path);
            {
                WriteTransaction wt(db);
                TableRef table = wt.get_group().add_table_with_primary_key("class_table", type_Int, "id");
                table->add_column(type_Int, "server_index");
                table->add_column(type_Int, "barq_index");
                table->add_column(type_Int, "file_index");
                table->add_column(type_Int, "session_index");
                table->add_column(type_Int, "transact_index");
                wt.commit();
            }
            std::string server_path = "/" + std::to_string(barq_index);
            for (int i = 0; i < num_sessions_per_file; ++i) {
                int client_index = 0;
                Session session = fixture.make_session(client_index, server_index, db, server_path);
                for (int j = 0; j < num_transacts_per_session; ++j) {
                    WriteTransaction wt(db);
                    TableRef table = wt.get_table("class_table");
                    Obj obj = table->create_object_with_primary_key(id.fetch_add(1));
                    obj.set("server_index", server_index);
                    obj.set("barq_index", barq_index);
                    obj.set("file_index", file_index);
                    obj.set("session_index", i);
                    obj.set("transact_index", j);
                    wt.commit();
                }
                session.wait_for_upload_complete_or_client_stopped();
            }
        }
        catch (...) {
            fixture.stop();
            throw;
        }
    };

    auto finish_download = [&](int server_index, int barq_index, int file_index) {
        try {
            int client_index = 0;
            std::string path = get_file_path(server_index, barq_index, file_index);
            DBRef db = DB::create(make_client_replication(), path);
            std::string server_path = "/" + std::to_string(barq_index);
            Session session = fixture.make_session(client_index, server_index, db, server_path);
            session.wait_for_download_complete_or_client_stopped();
        }
        catch (...) {
            fixture.stop();
            throw;
        }
    };

    // Make and upload changes
    {
        ThreadWrapper threads[num_servers][num_barqs_per_server][num_files_per_barq];
        for (int i = 0; i < num_servers; ++i) {
            for (int j = 0; j < num_barqs_per_server; ++j) {
                for (int k = 0; k < num_files_per_barq; ++k)
                    threads[i][j][k].start([=] {
                        run(i, j, k);
                    });
            }
        }
        for (size_t i = 0; i < num_servers; ++i) {
            for (size_t j = 0; j < num_barqs_per_server; ++j) {
                for (size_t k = 0; k < num_files_per_barq; ++k)
                    CHECK_NOT(threads[i][j][k].join());
            }
        }
    }

    // Finish downloading
    {
        ThreadWrapper threads[num_servers][num_barqs_per_server][num_files_per_barq];
        for (int i = 0; i < num_servers; ++i) {
            for (int j = 0; j < num_barqs_per_server; ++j) {
                for (int k = 0; k < num_files_per_barq; ++k)
                    threads[i][j][k].start([=] {
                        finish_download(i, j, k);
                    });
            }
        }
        for (size_t i = 0; i < num_servers; ++i) {
            for (size_t j = 0; j < num_barqs_per_server; ++j) {
                for (size_t k = 0; k < num_files_per_barq; ++k)
                    CHECK_NOT(threads[i][j][k].join());
            }
        }
    }

    // Check that all client side Barqs have been correctly synchronized
    std::set<std::tuple<int, int, int>> expected_rows;
    for (int i = 0; i < num_files_per_barq; ++i) {
        for (int j = 0; j < num_sessions_per_file; ++j) {
            for (int k = 0; k < num_transacts_per_session; ++k)
                expected_rows.emplace(i, j, k);
        }
    }
    for (size_t i = 0; i < num_servers; ++i) {
        for (size_t j = 0; j < num_barqs_per_server; ++j) {
            BARQ_ASSERT(num_files_per_barq > 0);
            int file_index_0 = 0;
            std::string path_0 = get_file_path(int(i), int(j), file_index_0);
            std::unique_ptr<Replication> history_0 = make_client_replication();
            DBRef db_0 = DB::create(*history_0, path_0);
            ReadTransaction rt_0(db_0);
            {
                ConstTableRef table = rt_0.get_table("class_table");
                if (CHECK(table)) {
                    std::set<std::tuple<int, int, int>> rows;
                    for (const Obj& obj : *table) {
                        int server_index = int(obj.get<int64_t>("server_index"));
                        int barq_index = int(obj.get<int64_t>("barq_index"));
                        int file_index = int(obj.get<int64_t>("file_index"));
                        int session_index = int(obj.get<int64_t>("session_index"));
                        int transact_index = int(obj.get<int64_t>("transact_index"));
                        CHECK_EQUAL(i, server_index);
                        CHECK_EQUAL(j, barq_index);
                        rows.emplace(file_index, session_index, transact_index);
                    }
                    CHECK(rows == expected_rows);
                }
            }
            for (int k = 1; k < num_files_per_barq; ++k) {
                std::string path = get_file_path(int(i), int(j), k);
                DBRef db = DB::create(make_client_replication(), path);
                ReadTransaction rt(db);
                CHECK(compare_groups(rt_0, rt));
            }
        }
    }
}


TEST_IF(Sync_ReadOnlyClient, false)
{
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    TEST_DIR(server_dir);
    MultiClientServerFixture fixture(2, 1, server_dir, test_context);
    bool did_get_permission_denied = false;
    fixture.set_client_side_error_handler(1, [&](Status status, bool) {
        CHECK_EQUAL(status, ErrorCodes::SyncPermissionDenied);
        did_get_permission_denied = true;
        fixture.get_client(1).shutdown();
    });
    fixture.start();

    // Write some stuff from the client that can upload
    {
        Session session_1 = fixture.make_bound_session(0, db_1, 0, "/test");
        WriteTransaction wt(db_1);
        auto table = wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
        table->add_column(type_Int, "i");
        table->create_object_with_primary_key(1);
        table->begin()->set("i", 123);
        wt.commit();
        session_1.wait_for_upload_complete_or_client_stopped();
    }

    // Check that the stuff was received on the read-only client
    {
        Session session_2 = fixture.make_bound_session(1, db_2, 0, "/test", g_signed_test_user_token_readonly);
        session_2.wait_for_download_complete_or_client_stopped();
        {
            ReadTransaction rt(db_2);
            auto table = rt.get_table("class_foo");
            CHECK_EQUAL(table->begin()->get<Int>("i"), 123);
        }
        // Try to upload something
        {
            WriteTransaction wt(db_2);
            auto table = wt.get_table("class_foo");
            table->begin()->set("i", 456);
            wt.commit();
        }
        session_2.wait_for_upload_complete_or_client_stopped();
        CHECK(did_get_permission_denied);
    }

    // Check that the original client was unchanged
    {
        Session session_1 = fixture.make_bound_session(0, db_1, 0, "/test");
        session_1.wait_for_download_complete_or_client_stopped();
        ReadTransaction rt(db_1);
        auto table = rt.get_table("class_foo");
        CHECK_EQUAL(table->begin()->get<Int>("i"), 123);
    }
}


// This test is a performance study. A single client keeps creating
// transactions that creates new objects and uploads them. The time to perform
// upload completion is measured and logged at info level.
TEST(Sync_SingleClientUploadForever_CreateObjects)
{
    int_fast32_t number_of_transactions = 100; // Set to low number in ordinary testing.

    util::Logger& logger = *test_context.logger;

    logger.info("Sync_SingleClientUploadForever_CreateObjects test. Number of transactions = %1",
                number_of_transactions);

    TEST_DIR(server_dir);
    TEST_CLIENT_DB(db);

    ClientServerFixture fixture(server_dir, test_context);
    fixture.start();

    ColKey col_int;
    ColKey col_str;
    ColKey col_dbl;
    ColKey col_time;

    {
        WriteTransaction wt{db};
        TableRef tr = wt.get_group().add_table_with_primary_key("class_table", type_Int, "id");
        col_int = tr->add_column(type_Int, "integer column");
        col_str = tr->add_column(type_String, "string column");
        col_dbl = tr->add_column(type_Double, "double column");
        col_time = tr->add_column(type_Timestamp, "timestamp column");
        wt.commit();
    }

    Session session = fixture.make_bound_session(db);
    session.wait_for_upload_complete_or_client_stopped();

    for (int_fast32_t i = 0; i < number_of_transactions; ++i) {
        WriteTransaction wt{db};
        TableRef tr = wt.get_table("class_table");
        auto obj = tr->create_object_with_primary_key(i);
        int_fast32_t number = i;
        obj.set<Int>(col_int, number);
        std::string str = "str: " + std::to_string(number);
        StringData str_data = StringData(str);
        obj.set(col_str, str_data);
        obj.set(col_dbl, double(number));
        obj.set(col_time, Timestamp{123, 456});
        wt.commit();
        auto before_upload = std::chrono::steady_clock::now();
        session.wait_for_upload_complete_or_client_stopped();
        auto after_upload = std::chrono::steady_clock::now();

        // We only log the duration every 1000 transactions. The duration is for a single changeset.
        if (i % 1000 == 0) {
            auto duration =
                std::chrono::duration_cast<std::chrono::milliseconds>(after_upload - before_upload).count();
            logger.info("Duration of single changeset upload(%1) = %2 ms", i, duration);
        }
    }
}


// This test is a performance study. A single client keeps creating
// transactions that changes the value of an existing object and uploads them.
// The time to perform upload completion is measured and logged at info level.
TEST(Sync_SingleClientUploadForever_MutateObject)
{
    int_fast32_t number_of_transactions = 100; // Set to low number in ordinary testing.

    util::Logger& logger = *test_context.logger;

    logger.info("Sync_SingleClientUploadForever_MutateObject test. Number of transactions = %1",
                number_of_transactions);

    TEST_DIR(server_dir);
    TEST_CLIENT_DB(db);

    ClientServerFixture fixture(server_dir, test_context);
    fixture.start();

    ColKey col_int;
    ColKey col_str;
    ColKey col_dbl;
    ColKey col_time;
    ObjKey obj_key;

    {
        WriteTransaction wt{db};
        TableRef tr = wt.get_group().add_table_with_primary_key("class_table", type_Int, "id");
        col_int = tr->add_column(type_Int, "integer column");
        col_str = tr->add_column(type_String, "string column");
        col_dbl = tr->add_column(type_Double, "double column");
        col_time = tr->add_column(type_Timestamp, "timestamp column");
        obj_key = tr->create_object_with_primary_key(1).get_key();
        wt.commit();
    }

    Session session = fixture.make_bound_session(db);
    session.wait_for_upload_complete_or_client_stopped();

    for (int_fast32_t i = 0; i < number_of_transactions; ++i) {
        WriteTransaction wt{db};
        TableRef tr = wt.get_table("class_table");
        int_fast32_t number = i;
        auto obj = tr->get_object(obj_key);
        obj.set<Int>(col_int, number);
        std::string str = "str: " + std::to_string(number);
        StringData str_data = StringData(str);
        obj.set(col_str, str_data);
        obj.set(col_dbl, double(number));
        obj.set(col_time, Timestamp{123, 456});
        wt.commit();
        auto before_upload = std::chrono::steady_clock::now();
        session.wait_for_upload_complete_or_client_stopped();
        auto after_upload = std::chrono::steady_clock::now();

        // We only log the duration every 1000 transactions. The duration is for a single changeset.
        if (i % 1000 == 0) {
            auto duration =
                std::chrono::duration_cast<std::chrono::milliseconds>(after_upload - before_upload).count();
            logger.info("Duration of single changeset upload(%1) = %2 ms", i, duration);
        }
    }
}


// This test is used to time upload and download.
// The test might be moved to a performance test directory later.
TEST(Sync_LargeUploadDownloadPerformance)
{
    int_fast32_t number_of_transactions = 2;         // Set to low number in ordinary testing.
    int_fast32_t number_of_rows_per_transaction = 5; // Set to low number in ordinary testing.
    int number_of_download_clients = 1;              // Set to low number in ordinary testing
    bool print_durations = false;                    // Set to false in ordinary testing.

    if (print_durations) {
        std::cerr << "Number of transactions = " << number_of_transactions << std::endl;
        std::cerr << "Number of rows per transaction = " << number_of_rows_per_transaction << std::endl;
        std::cerr << "Number of download clients = " << number_of_download_clients << std::endl;
    }

    TEST_DIR(server_dir);
    ClientServerFixture fixture(server_dir, test_context);
    fixture.start();

    TEST_CLIENT_DB(db_upload);

    // Populate path_upload barq with data.
    auto start_data_creation = std::chrono::steady_clock::now();
    {
        {
            WriteTransaction wt{db_upload};
            TableRef tr = wt.get_group().add_table_with_primary_key("class_table", type_Int, "id");
            tr->add_column(type_Int, "integer column");
            tr->add_column(type_String, "string column");
            tr->add_column(type_Double, "double column");
            tr->add_column(type_Timestamp, "timestamp column");
            wt.commit();
        }

        for (int_fast32_t i = 0; i < number_of_transactions; ++i) {
            WriteTransaction wt{db_upload};
            TableRef tr = wt.get_table("class_table");
            for (int_fast32_t j = 0; j < number_of_rows_per_transaction; ++j) {
                Obj obj = tr->create_object_with_primary_key(i);
                int_fast32_t number = i * number_of_rows_per_transaction + j;
                obj.set("integer column", number);
                std::string str = "str: " + std::to_string(number);
                StringData str_data = StringData(str);
                obj.set("string column", str_data);
                obj.set("double column", double(number));
                obj.set("timestamp column", Timestamp{123, 456});
            }
            wt.commit();
        }
    }
    auto end_data_creation = std::chrono::steady_clock::now();
    auto duration_data_creation =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_data_creation - start_data_creation).count();
    if (print_durations)
        std::cerr << "Duration of data creation = " << duration_data_creation << " ms" << std::endl;

    // Upload the data.
    auto start_session_upload = std::chrono::steady_clock::now();

    Session session_upload = fixture.make_bound_session(db_upload);
    session_upload.wait_for_upload_complete_or_client_stopped();

    auto end_session_upload = std::chrono::steady_clock::now();
    auto duration_upload =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_session_upload - start_session_upload).count();
    if (print_durations)
        std::cerr << "Duration of uploading = " << duration_upload << " ms" << std::endl;


    // Download the data to the download barqs.
    auto start_sesion_download = std::chrono::steady_clock::now();

    std::vector<DBTestPathGuard> shared_group_test_path_guards;
    std::vector<DBRef> dbs;
    std::vector<Session> sessions;

    for (int i = 0; i < number_of_download_clients; ++i) {
        std::string path = get_test_path(test_context.get_test_name(), std::to_string(i));
        shared_group_test_path_guards.emplace_back(path);
        dbs.push_back(DB::create(make_client_replication(), path));
        sessions.push_back(fixture.make_bound_session(dbs.back()));
    }

    // Wait for all Barqs to finish. They might finish in another order than
    // started, but calling download_complete on a client after it finished only
    // adds a tiny amount of extra mark messages.
    for (auto& session : sessions)
        session.wait_for_download_complete_or_client_stopped();


    auto end_session_download = std::chrono::steady_clock::now();
    auto duration_download =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_session_download - start_sesion_download).count();
    if (print_durations)
        std::cerr << "Duration of downloading = " << duration_download << " ms" << std::endl;


    // Check convergence.
    for (int i = 0; i < number_of_download_clients; ++i) {
        ReadTransaction rt_1(db_upload);
        ReadTransaction rt_2(dbs[i]);
        CHECK(compare_groups(rt_1, rt_2, *test_context.logger));
    }
}


// This test creates a changeset that is larger than 4GB, uploads it and downloads it to another client.
// The test checks that compression and other aspects of large changeset handling works.
// The test is disabled since it requires a powerful machine to run.
TEST_IF(Sync_4GB_Messages, false)
{
    // The changeset will be slightly larger.
    const uint64_t approximate_changeset_size = uint64_t(1) << 32;

    TEST_DIR(dir);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);
    ClientServerFixture fixture(dir, test_context);
    fixture.start();

    Session session_1 = fixture.make_bound_session(db_1);
    session_1.wait_for_download_complete_or_client_stopped();

    Session session_2 = fixture.make_bound_session(db_2);
    session_2.wait_for_download_complete_or_client_stopped();

    const size_t single_object_data_size = size_t(1e7); // 10 MB which is below the 16 MB limit
    const int num_objects = approximate_changeset_size / single_object_data_size + 1;

    const std::string str_a(single_object_data_size, 'a');
    BinaryData bd_a(str_a.data(), single_object_data_size);

    const std::string str_b(single_object_data_size, 'b');
    BinaryData bd_b(str_b.data(), single_object_data_size);

    const std::string str_c(single_object_data_size, 'c');
    BinaryData bd_c(str_c.data(), single_object_data_size);

    {
        WriteTransaction wt{db_1};

        TableRef tr = wt.get_group().add_table_with_primary_key("class_simple_data", type_Int, "id");
        auto col_key = tr->add_column(type_Binary, "binary column");
        for (int i = 0; i < num_objects; ++i) {
            Obj obj = tr->create_object_with_primary_key(i);
            switch (i % 3) {
                case 0:
                    obj.set(col_key, bd_a);
                    break;
                case 1:
                    obj.set(col_key, bd_b);
                    break;
                default:
                    obj.set(col_key, bd_c);
            }
        }
        wt.commit();
    }
    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();

    // Check convergence.
    {
        ReadTransaction rt_1(db_1);
        ReadTransaction rt_2(db_2);
        CHECK(compare_groups(rt_1, rt_2, *test_context.logger));
    }
}


TEST(Sync_RefreshSignedUserToken)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db);
    ClientServerFixture fixture(dir, test_context);
    fixture.start();

    Session session = fixture.make_bound_session(db);
    session.wait_for_download_complete_or_client_stopped();
    session.refresh(g_signed_test_user_token);
    session.wait_for_download_complete_or_client_stopped();
}


// This test refreshes the user token multiple times right after binding
// the session. The test tries to achieve a situation where a session is
// enlisted to send after sending BIND but before receiving ALLOC.
// The token is refreshed multiple times to increase the probability that the
// refresh took place after BIND. The check of the test is just the absence of
// errors.
TEST(Sync_RefreshRightAfterBind)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db);
    ClientServerFixture fixture(dir, test_context);
    fixture.start();

    Session session = fixture.make_bound_session(db);
    for (int i = 0; i < 50; ++i) {
        session.refresh(g_signed_test_user_token_readonly);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    session.wait_for_download_complete_or_client_stopped();
}


TEST(Sync_Permissions)
{
    TEST_CLIENT_DB(db_valid);

    bool did_see_error_for_valid = false;

    TEST_DIR(server_dir);

    ClientServerFixture fixture{server_dir, test_context};
    fixture.set_client_side_error_handler([&](Status status, bool) {
        CHECK_EQUAL("", status.reason());
        did_see_error_for_valid = true;
    });
    fixture.start();

    Session session_valid = fixture.make_bound_session(db_valid, "/valid", g_signed_test_user_token_for_path);

    write_transaction(db_valid, [](WriteTransaction& wt) {
        wt.get_group().add_table_with_primary_key("class_a", type_Int, "id");
    });

    auto completed = session_valid.wait_for_upload_complete_or_client_stopped();
    CHECK_NOT(did_see_error_for_valid);
    CHECK(completed);
}


#if BARQ_HAVE_OPENSSL

// This test is used to verify the ssl_verify_callback function against an
// external server. The tests should only be used for debugging should normally
// be disabled.
TEST_IF(Sync_SSL_Certificate_Verify_Callback_External, false)
{
    const std::string server_address = "www.writeurl.com";
    Session::port_type port = 443;

    TEST_CLIENT_DB(db);

    Client::Config config;
    config.logger = std::make_shared<util::PrefixLogger>("Client: ", test_context.logger);
    auto socket_provider = std::make_shared<websocket::DefaultSocketProvider>(config.logger, "");
    config.socket_provider = socket_provider;
    config.reconnect_mode = ReconnectMode::testing;
    Client client(config);

    auto ssl_verify_callback = [&](const std::string server_address, Session::port_type server_port,
                                   const char* pem_data, size_t pem_size, int preverify_ok, int depth) {
        StringData pem{pem_data, pem_size};
        test_context.logger->info("server_address = %1, server_port = %2, pem =\n%3\n, "
                                  " preverify_ok = %4, depth = %5",
                                  server_address, server_port, pem, preverify_ok, depth);
        if (depth == 0)
            client.shutdown();
        return true;
    };

    Session::Config session_config;
    session_config.server_address = server_address;
    session_config.server_port = port;
    session_config.protocol_envelope = ProtocolEnvelope::barqs;
    session_config.verify_servers_ssl_certificate = true;
    session_config.ssl_trust_certificate_path = util::none;
    session_config.ssl_verify_callback = ssl_verify_callback;

    Session session(client, db, nullptr, nullptr, std::move(session_config));
    session.wait_for_download_complete_or_client_stopped();

    client.shutdown_and_wait();
}

#endif // BARQ_HAVE_OPENSSL


// This test has a single client connected to a server with
// one session.
// The client creates four changesets at various times and
// uploads them to the server. The session has a registered
// progress_handler. It is checked that downloaded_bytes,
// downloadable_bytes, uploaded_bytes, and uploadable_bytes
// are correct. This client does not have any downloaded_bytes
// or downloadable bytes because it created all the changesets
// itself.
TEST(Sync_UploadDownloadProgress_1)
{
    TEST_DIR(server_dir);
    TEST_CLIENT_DB(db);

    struct ProgressInfo {
        uint64_t downloaded_bytes = 0;
        uint64_t downloadable_bytes = 0;
        uint64_t uploaded_bytes = 0;
        uint64_t uploadable_bytes = 0;
        uint64_t snapshot_version = 0;
    };

    ProgressInfo first_run_progress;

    {
        ClientServerFixture fixture(server_dir, test_context);
        fixture.start();

        Session::Config config;
        std::mutex progress_mutex;
        std::condition_variable progress_cv;
        std::optional<ProgressInfo> observed_progress;
        auto wait_for_progress_info = [&] {
            std::unique_lock lk(progress_mutex);
            progress_cv.wait(lk, [&] {
                return observed_progress;
            });
            auto ret = std::exchange(observed_progress, std::optional<ProgressInfo>{});
            return *ret;
        };
        config.progress_handler = [&](uint64_t downloaded, uint64_t downloadable, uint64_t uploaded,
                                      uint64_t uploadable, uint64_t snapshot, double, double, int64_t) {
            std::lock_guard lk(progress_mutex);
            observed_progress = ProgressInfo{downloaded, downloadable, uploaded, uploadable, snapshot};
            progress_cv.notify_one();
        };


        Session session = fixture.make_session(db, "/test", std::move(config));
        auto progress_info = wait_for_progress_info();

        CHECK_EQUAL(progress_info.downloaded_bytes, uint_fast64_t(0));
        CHECK_EQUAL(progress_info.downloadable_bytes, uint_fast64_t(0));
        CHECK_EQUAL(progress_info.uploaded_bytes, uint_fast64_t(0));
        CHECK_EQUAL(progress_info.uploadable_bytes, uint_fast64_t(0));
        CHECK_GREATER_EQUAL(progress_info.snapshot_version, 1);

        auto commit_version = write_transaction(db, [](WriteTransaction& wt) {
            auto tr = wt.get_group().add_table_with_primary_key("class_table", type_Int, "id");
            tr->add_column(type_Int, "integer column");
        });

        session.wait_for_upload_complete_or_client_stopped();
        session.wait_for_download_complete_or_client_stopped();

        auto old_progress_info = progress_info;
        progress_info = wait_for_progress_info();
        CHECK_EQUAL(progress_info.downloaded_bytes, uint_fast64_t(0));
        CHECK_EQUAL(progress_info.downloadable_bytes, uint_fast64_t(0));
        CHECK_GREATER(progress_info.uploaded_bytes, old_progress_info.uploaded_bytes);
        CHECK_GREATER(progress_info.uploadable_bytes, old_progress_info.uploadable_bytes);
        CHECK_GREATER_EQUAL(progress_info.snapshot_version, commit_version);

        commit_version = write_transaction(db, [](WriteTransaction& wt) {
            wt.get_table("class_table")->create_object_with_primary_key(1).set("integer column", 42);
        });

        session.wait_for_upload_complete_or_client_stopped();
        session.wait_for_download_complete_or_client_stopped();

        old_progress_info = progress_info;
        progress_info = wait_for_progress_info();
        CHECK_EQUAL(progress_info.downloaded_bytes, uint_fast64_t(0));
        CHECK_EQUAL(progress_info.downloadable_bytes, uint_fast64_t(0));
        CHECK_GREATER(progress_info.uploaded_bytes, old_progress_info.uploaded_bytes);
        CHECK_GREATER(progress_info.uploadable_bytes, old_progress_info.uploadable_bytes);
        CHECK_GREATER_EQUAL(progress_info.snapshot_version, commit_version);
        first_run_progress = progress_info;
    }

    {
        // Here we check that the progress handler is called
        // after the session is bound, and that the values
        // are the ones stored in the Barq in the previous
        // session.

        ClientServerFixture fixture(server_dir, test_context);
        fixture.start();

        int number_of_handler_calls = 0;
        auto pf = util::make_promise_future<int>();
        Session::Config config;
        config.progress_handler = [&](uint64_t downloaded, uint64_t downloadable, uint64_t uploaded,
                                      uint64_t uploadable, uint64_t snapshot, double, double, int64_t) {
            CHECK_EQUAL(downloaded, first_run_progress.downloaded_bytes);
            CHECK_EQUAL(downloadable, first_run_progress.downloadable_bytes);
            CHECK_EQUAL(uploaded, first_run_progress.uploaded_bytes);
            CHECK_EQUAL(uploadable, first_run_progress.uploadable_bytes);
            CHECK_GREATER(snapshot, first_run_progress.snapshot_version);
            number_of_handler_calls++;
            pf.promise.emplace_value(number_of_handler_calls);
        };

        Session session = fixture.make_session(db, "/test", std::move(config));
        CHECK_EQUAL(pf.future.get(), 1);
    }
}


// This test creates one server and a client with
// two sessions that synchronizes with the same server Barq.
// The clients generate changesets, uploads and downloads, and
// waits for upload/download completion. Both sessions have a
// progress handler registered, and it is checked that the
// progress handlers report the correct values.
TEST(Sync_UploadDownloadProgress_2)
{
    TEST_DIR(server_dir);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    ClientServerFixture fixture(server_dir, test_context);
    fixture.start();

    uint_fast64_t downloaded_bytes_1 = 123; // Not zero
    uint_fast64_t downloadable_bytes_1 = 123;
    uint_fast64_t uploaded_bytes_1 = 123;
    uint_fast64_t uploadable_bytes_1 = 123;
    uint_fast64_t snapshot_version_1 = 0;

    Session::Config config_1;
    config_1.progress_handler = [&](uint64_t downloaded_bytes, uint64_t downloadable_bytes, uint64_t uploaded_bytes,
                                    uint64_t uploadable_bytes, uint64_t snapshot_version, double, double, int64_t) {
        downloaded_bytes_1 = downloaded_bytes;
        downloadable_bytes_1 = downloadable_bytes;
        uploaded_bytes_1 = uploaded_bytes;
        uploadable_bytes_1 = uploadable_bytes;
        snapshot_version_1 = snapshot_version;
    };

    uint_fast64_t downloaded_bytes_2 = 123;
    uint_fast64_t downloadable_bytes_2 = 123;
    uint_fast64_t uploaded_bytes_2 = 123;
    uint_fast64_t uploadable_bytes_2 = 123;
    uint_fast64_t snapshot_version_2 = 0;

    Session::Config config_2;
    config_2.progress_handler = [&](uint64_t downloaded_bytes, uint64_t downloadable_bytes, uint64_t uploaded_bytes,
                                    uint64_t uploadable_bytes, uint64_t snapshot_version, double, double, int64_t) {
        downloaded_bytes_2 = downloaded_bytes;
        downloadable_bytes_2 = downloadable_bytes;
        uploaded_bytes_2 = uploaded_bytes;
        uploadable_bytes_2 = uploadable_bytes;
        snapshot_version_2 = snapshot_version;
    };

    Session session_1 = fixture.make_session(db_1, "/test", std::move(config_1));
    Session session_2 = fixture.make_session(db_2, "/test", std::move(config_2));

    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_upload_complete_or_client_stopped();
    session_1.wait_for_download_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();

    CHECK_EQUAL(downloaded_bytes_1, downloadable_bytes_1);
    CHECK_EQUAL(downloaded_bytes_2, downloadable_bytes_2);
    CHECK_EQUAL(downloaded_bytes_1, downloaded_bytes_2);
    CHECK_EQUAL(downloadable_bytes_1, 0);
    CHECK_GREATER(snapshot_version_1, 0);

    CHECK_EQUAL(uploaded_bytes_1, 0);
    CHECK_EQUAL(uploadable_bytes_1, 0);

    CHECK_EQUAL(uploaded_bytes_2, 0);
    CHECK_EQUAL(uploadable_bytes_2, 0);
    CHECK_GREATER(snapshot_version_2, 0);

    write_transaction(db_1, [](WriteTransaction& wt) {
        TableRef tr = wt.get_group().add_table_with_primary_key("class_table", type_Int, "id");
        tr->add_column(type_Int, "integer column");
    });

    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_upload_complete_or_client_stopped();
    session_1.wait_for_download_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();

    CHECK_EQUAL(downloaded_bytes_1, 0);
    CHECK_EQUAL(downloadable_bytes_1, 0);

    CHECK_NOT_EQUAL(downloaded_bytes_2, 0);
    CHECK_NOT_EQUAL(downloadable_bytes_2, 0);

    CHECK_NOT_EQUAL(uploaded_bytes_1, 0);
    CHECK_NOT_EQUAL(uploadable_bytes_1, 0);

    CHECK_EQUAL(uploaded_bytes_2, 0);
    CHECK_EQUAL(uploadable_bytes_2, 0);

    CHECK_GREATER(snapshot_version_1, 1);
    CHECK_GREATER(snapshot_version_2, 1);

    write_transaction(db_1, [](WriteTransaction& wt) {
        TableRef tr = wt.get_table("class_table");
        tr->create_object_with_primary_key(1).set("integer column", 42);
    });

    write_transaction(db_1, [](WriteTransaction& wt) {
        TableRef tr = wt.get_table("class_table");
        tr->create_object_with_primary_key(2).set("integer column", 44);
    });

    write_transaction(db_2, [](WriteTransaction& wt) {
        TableRef tr = wt.get_table("class_table");
        tr->create_object_with_primary_key(3).set("integer column", 43);
    });

    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_upload_complete_or_client_stopped();
    session_1.wait_for_download_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();

    CHECK_NOT_EQUAL(downloaded_bytes_1, 0);
    CHECK_NOT_EQUAL(downloadable_bytes_1, 0);

    CHECK_NOT_EQUAL(downloaded_bytes_2, 0);
    CHECK_NOT_EQUAL(downloadable_bytes_2, 0);

    CHECK_NOT_EQUAL(uploaded_bytes_1, 0);
    CHECK_NOT_EQUAL(uploadable_bytes_1, 0);

    CHECK_NOT_EQUAL(uploaded_bytes_2, 0);
    CHECK_NOT_EQUAL(uploadable_bytes_2, 0);

    CHECK_GREATER(snapshot_version_1, 4);
    CHECK_GREATER(snapshot_version_2, 3);

    write_transaction(db_1, [](WriteTransaction& wt) {
        TableRef tr = wt.get_table("class_table");
        tr->begin()->set("integer column", 101);
    });

    write_transaction(db_2, [](WriteTransaction& wt) {
        TableRef tr = wt.get_table("class_table");
        tr->begin()->set("integer column", 102);
    });

    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_upload_complete_or_client_stopped();
    session_1.wait_for_download_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();

    CHECK_EQUAL(downloaded_bytes_1, downloadable_bytes_1);

    // uncertainty due to merge
    CHECK_NOT_EQUAL(downloaded_bytes_1, 0);

    CHECK_EQUAL(downloaded_bytes_2, downloadable_bytes_2);
    CHECK_NOT_EQUAL(downloaded_bytes_2, 0);

    CHECK_NOT_EQUAL(uploaded_bytes_1, 0);
    CHECK_NOT_EQUAL(uploadable_bytes_1, 0);

    CHECK_NOT_EQUAL(uploaded_bytes_2, 0);
    CHECK_NOT_EQUAL(uploadable_bytes_2, 0);

    CHECK_GREATER(snapshot_version_1, 6);
    CHECK_GREATER(snapshot_version_2, 5);

    CHECK_GREATER(snapshot_version_1, 6);
    CHECK_GREATER(snapshot_version_2, 5);

    // Check convergence.
    {
        ReadTransaction rt_1(db_1);
        ReadTransaction rt_2(db_2);
        CHECK(compare_groups(rt_1, rt_2, *test_context.logger));
    }
}


// This test creates a server and a client. Initially, the server is not running.
// The client generates changes and binds a session. It is verified that the
// progress_handler() is called and that the four arguments of progress_handler()
// have the correct values. The server is started in the first call to
// progress_handler() and it is checked that after upload and download completion,
// the upload_progress_handler has been called again, and that the four arguments
// have the correct values. After this, the server is stopped and the client produces
// more changes. It is checked that the progress_handler() is called and that the
// final values are correct.
TEST(Sync_UploadDownloadProgress_3)
{
    TEST_DIR(server_dir);
    TEST_CLIENT_DB(db);

    std::string server_address = "localhost";

    Server::Config server_config;
    server_config.logger = std::make_shared<util::PrefixLogger>("Server: ", test_context.logger);
    server_config.listen_address = server_address;
    server_config.listen_port = "";
    server_config.tcp_no_delay = true;

    util::Optional<PKey> public_key = PKey::load_public(test_server_key_path());
    Server server(server_dir, std::move(public_key), server_config);
    server.start();
    auto server_port = server.listen_endpoint().port();

    ThreadWrapper server_thread;

    // The server is not running.

    {
        WriteTransaction wt{db};
        TableRef tr = wt.get_group().add_table_with_primary_key("class_table", type_Int, "id");
        tr->add_column(type_Int, "integer column");
        wt.commit();
    }

    Client::Config client_config;
    client_config.logger = std::make_shared<util::PrefixLogger>("Client: ", test_context.logger);
    auto socket_provider = std::make_shared<websocket::DefaultSocketProvider>(client_config.logger, "");
    client_config.socket_provider = socket_provider;
    client_config.reconnect_mode = ReconnectMode::testing;
    Client client(client_config);

    // entry is used to count the number of calls to
    // progress_handler. At the first call, the server is
    // not running, and it is started by progress_handler().

    bool should_signal_cond_var = false;
    auto signal_pf = util::make_promise_future<void>();

    uint_fast64_t downloaded_bytes_1 = 123; // Not zero
    uint_fast64_t downloadable_bytes_1 = 123;
    uint_fast64_t uploaded_bytes_1 = 123;
    uint_fast64_t uploadable_bytes_1 = 123;
    uint_fast64_t snapshot_version_1 = 0;

    Session::Config config;
    config.service_identifier = "/barq-sync";
    config.server_address = server_address;
    config.signed_user_token = g_signed_test_user_token;
    config.server_port = server_port;
    config.barq_identifier = "/test";
    config.progress_handler = [&, entry = 0](uint_fast64_t downloaded_bytes, uint_fast64_t downloadable_bytes,
                                             uint_fast64_t uploaded_bytes, uint_fast64_t uploadable_bytes,
                                             uint_fast64_t snapshot_version, double, double, int64_t) mutable {
        downloaded_bytes_1 = downloaded_bytes;
        downloadable_bytes_1 = downloadable_bytes;
        uploaded_bytes_1 = uploaded_bytes;
        uploadable_bytes_1 = uploadable_bytes;
        snapshot_version_1 = snapshot_version;

        if (entry == 0) {
            CHECK_EQUAL(downloaded_bytes, 0);
            CHECK_EQUAL(downloadable_bytes, 0);
            CHECK_EQUAL(uploaded_bytes, 0);
            CHECK_NOT_EQUAL(uploadable_bytes, 0);
            CHECK_EQUAL(snapshot_version, 4);
        }

        if (should_signal_cond_var) {
            signal_pf.promise.emplace_value();
        }

        entry++;
    };

    server_thread.start([&] {
        server.run();
    });

    Session session(client, db, nullptr, nullptr, std::move(config));

    session.wait_for_upload_complete_or_client_stopped();
    session.wait_for_download_complete_or_client_stopped();

    // Now the server is running.

    CHECK_EQUAL(downloaded_bytes_1, 0);
    CHECK_EQUAL(downloadable_bytes_1, 0);
    CHECK_NOT_EQUAL(uploaded_bytes_1, 0);
    CHECK_NOT_EQUAL(uploadable_bytes_1, 0);
    CHECK_GREATER_EQUAL(snapshot_version_1, 2);

    server.stop();

    // The server is stopped

    should_signal_cond_var = true;

    uint_fast64_t commited_version;
    {
        WriteTransaction wt{db};
        TableRef tr = wt.get_table("class_table");
        tr->create_object_with_primary_key(123).set("integer column", 42);
        commited_version = wt.commit();
    }

    signal_pf.future.get();

    CHECK_EQUAL(downloaded_bytes_1, 0);
    CHECK_EQUAL(downloadable_bytes_1, 0);
    CHECK_NOT_EQUAL(uploaded_bytes_1, 0);
    CHECK_NOT_EQUAL(uploadable_bytes_1, 0);
    CHECK_EQUAL(snapshot_version_1, commited_version);

    server_thread.join();
}


// This test creates a server and two clients. The first client uploads two
// large changesets. The other client downloads them. The download messages to
// the second client contains one changeset because the changesets are larger
// than the soft size limit for changesets in the DOWNLOAD message. This implies
// that after receiving the first DOWNLOAD message, the second client will have
// downloaded_bytes < downloadable_bytes.
TEST(Sync_UploadDownloadProgress_4)
{
    TEST_DIR(server_dir);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    {
        WriteTransaction wt{db_1};
        TableRef tr = wt.get_group().add_table_with_primary_key("class_table", type_Int, "id");
        auto col = tr->add_column(type_Binary, "binary column");
        tr->create_object_with_primary_key(1);
        std::string str(size_t(5e5), 'a');
        BinaryData bd(str.data(), str.size());
        tr->begin()->set(col, bd);
        wt.commit();
    }

    {
        WriteTransaction wt{db_1};
        TableRef tr = wt.get_table("class_table");
        auto col = tr->get_column_key("binary column");
        tr->create_object_with_primary_key(2);
        std::string str(size_t(1e6), 'a');
        BinaryData bd(str.data(), str.size());
        tr->begin()->set(col, bd);
        wt.commit();
    }

    ClientServerFixture::Config config;
    config.max_download_size = size_t(1e5);
    ClientServerFixture fixture(server_dir, test_context, std::move(config));
    fixture.start();

    int entry_1 = 0;
    Session::Config config_1;
    config_1.progress_handler = [&](uint_fast64_t downloaded_bytes, uint_fast64_t downloadable_bytes,
                                    uint_fast64_t uploaded_bytes, uint_fast64_t uploadable_bytes,
                                    uint_fast64_t snapshot_version, double, double, int64_t) {
        CHECK_EQUAL(downloaded_bytes, 0);
        CHECK_EQUAL(downloadable_bytes, 0);
        CHECK_NOT_EQUAL(uploadable_bytes, 0);

        switch (entry_1) {
            case 0:
                // We've received the empty DOWNLOAD message and now have reliable
                // download progress
                CHECK_EQUAL(uploaded_bytes, 0);
                CHECK_EQUAL(snapshot_version, 5);
                break;

            case 1:
                // First UPLOAD is complete, but we still have more to upload
                // because the changesets are too large to batch into a single upload
                CHECK_GREATER(uploaded_bytes, 0);
                CHECK_LESS(uploaded_bytes, uploadable_bytes);
                CHECK_EQUAL(snapshot_version, 6);
                break;

            case 2:
                // Second UPLOAD is complete and we're done uploading
                CHECK_EQUAL(uploaded_bytes, uploadable_bytes);
                CHECK_EQUAL(snapshot_version, 7);
                break;
        }

        ++entry_1;
    };

    Session session_1 = fixture.make_session(db_1, "/test", std::move(config_1));
    session_1.wait_for_upload_complete_or_client_stopped();
    session_1.wait_for_download_complete_or_client_stopped();

    CHECK_EQUAL(entry_1, 3);

    int entry_2 = 0;

    Session::Config config_2;
    config_2.progress_handler = [&](uint_fast64_t downloaded_bytes, uint_fast64_t downloadable_bytes,
                                    uint_fast64_t uploaded_bytes, uint_fast64_t uploadable_bytes,
                                    uint_fast64_t snapshot_version, double, double, int64_t) {
        CHECK_EQUAL(uploaded_bytes, 0);
        CHECK_EQUAL(uploadable_bytes, 0);

        switch (entry_2) {
            case 0:
                // First DOWNLOAD message received. Some data is downloaded, but
                // download isn't compelte
                CHECK_GREATER(downloaded_bytes, 0);
                CHECK_GREATER(downloadable_bytes, 0);
                CHECK_LESS(downloaded_bytes, downloadable_bytes);
                CHECK_EQUAL(snapshot_version, 3);
                break;

            case 1:
                // Second DOWNLOAD message received. Download is now complete.
                CHECK_GREATER(downloaded_bytes, 0);
                CHECK_GREATER(downloadable_bytes, 0);
                CHECK_EQUAL(downloaded_bytes, downloadable_bytes);
                CHECK_EQUAL(snapshot_version, 4);
                break;
        }
        ++entry_2;
    };

    Session session_2 = fixture.make_session(db_2, "/test", std::move(config_2));

    session_2.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();
    CHECK_EQUAL(entry_2, 2);
}


// This test has a single client connected to a server with one session. The
// client does not create any changesets. The test verifies that the client gets
// a confirmation from the server of downloadable_bytes = 0.
TEST(Sync_UploadDownloadProgress_5)
{
    TEST_DIR(server_dir);
    TEST_CLIENT_DB(db);

    ClientServerFixture fixture(server_dir, test_context);
    fixture.start();

    auto pf = util::make_promise_future();
    Session::Config config;
    config.progress_handler = [&](uint_fast64_t downloaded_bytes, uint_fast64_t downloadable_bytes,
                                  uint_fast64_t uploaded_bytes, uint_fast64_t uploadable_bytes,
                                  uint_fast64_t snapshot_version, double, double, int64_t) {
        CHECK_EQUAL(downloaded_bytes, 0);
        CHECK_EQUAL(downloadable_bytes, 0);
        CHECK_EQUAL(uploaded_bytes, 0);
        CHECK_EQUAL(uploadable_bytes, 0);
        CHECK_EQUAL(snapshot_version, 3);
        pf.promise.emplace_value();
    };

    Session session = fixture.make_session(db, "/test", std::move(config));
    pf.future.get();

    // The check is that we reach this point.
}


// This test has a single client connected to a server with one session.
// The session has a registered progress handler.
TEST(Sync_UploadDownloadProgress_6)
{
    TEST_DIR(server_dir);
    TEST_CLIENT_DB(db);

    Server::Config server_config;
    server_config.logger = std::make_shared<util::PrefixLogger>("Server: ", test_context.logger);
    server_config.listen_address = "localhost";
    server_config.listen_port = "";
    server_config.tcp_no_delay = true;

    util::Optional<PKey> public_key = PKey::load_public(test_server_key_path());
    Server server(server_dir, std::move(public_key), server_config);
    server.start();

    auto server_port = server.listen_endpoint().port();

    ThreadWrapper server_thread;
    server_thread.start([&] {
        server.run();
    });

    Client::Config client_config;
    client_config.logger = std::make_shared<util::PrefixLogger>("Client: ", test_context.logger);
    auto socket_provider = std::make_shared<websocket::DefaultSocketProvider>(client_config.logger, "");
    client_config.socket_provider = socket_provider;
    client_config.reconnect_mode = ReconnectMode::testing;
    client_config.one_connection_per_session = false;
    Client client(client_config);

    util::ScopeExit cleanup([&]() noexcept {
        client.shutdown_and_wait();
        server.stop();
        server_thread.join();
    });

    auto session_pf = util::make_promise_future<std::unique_ptr<Session>*>();
    auto complete_pf = util::make_promise_future();
    Session::Config session_config;
    session_config.server_address = "localhost";
    session_config.server_port = server_port;
    session_config.barq_identifier = "/test";
    session_config.service_identifier = "/barq-sync";
    session_config.signed_user_token = g_signed_test_user_token;
    session_config.progress_handler = [&](uint_fast64_t downloaded_bytes, uint_fast64_t downloadable_bytes,
                                          uint_fast64_t uploaded_bytes, uint_fast64_t uploadable_bytes,
                                          uint_fast64_t snapshot_version, double, double, int64_t) {
        CHECK_EQUAL(downloaded_bytes, 0);
        CHECK_EQUAL(downloadable_bytes, 0);
        CHECK_EQUAL(uploaded_bytes, 0);
        CHECK_EQUAL(uploadable_bytes, 0);
        CHECK_EQUAL(snapshot_version, 3);
        session_pf.future.get()->reset();
        complete_pf.promise.emplace_value();
    };
    auto session = std::make_unique<Session>(client, db, nullptr, nullptr, std::move(session_config));
    session_pf.promise.emplace_value(&session);
    complete_pf.future.get();
    CHECK(!session);

    // The check is that we reach this point without deadlocking or throwing an assert while tearing
    // down the active session
}

// This test has a single client starting to connect to the server with one session.
// The client is torn down immediately after bind is called on the session.
// The session will still be active and has an unactualized session wrapper when the
// client is torn down, which leads to both calls to finalize_before_actualization() and
// and finalize().
TEST(Sync_UploadDownloadProgress_7)
{
    TEST_DIR(server_dir);
    TEST_CLIENT_DB(db);

    Server::Config server_config;
    server_config.logger = std::make_shared<util::PrefixLogger>("Server: ", test_context.logger);
    server_config.listen_address = "localhost";
    server_config.listen_port = "";
    server_config.tcp_no_delay = true;

    util::Optional<PKey> public_key = PKey::load_public(test_server_key_path());
    Server server(server_dir, std::move(public_key), server_config);
    server.start();

    auto server_port = server.listen_endpoint().port();

    ThreadWrapper server_thread;
    server_thread.start([&] {
        server.run();
    });

    Client::Config client_config;
    client_config.logger = std::make_shared<util::PrefixLogger>("Client: ", test_context.logger);
    auto socket_provider = std::make_shared<websocket::DefaultSocketProvider>(client_config.logger, "");
    client_config.socket_provider = socket_provider;
    client_config.reconnect_mode = ReconnectMode::testing;
    client_config.one_connection_per_session = false;
    Client client(client_config);

    Session::Config session_config;
    session_config.server_address = "localhost";
    session_config.server_port = server_port;
    session_config.barq_identifier = "/test";
    session_config.signed_user_token = g_signed_test_user_token;

    Session session(client, db, nullptr, nullptr, std::move(session_config));

    client.shutdown_and_wait();
    server.stop();
    server_thread.join();

    // The check is that we reach this point without deadlocking or throwing an assert while tearing
    // down the session that is in the process of being created.
}

TEST(Sync_UploadProgress_EmptyCommits)
{
    TEST_DIR(server_dir);
    TEST_CLIENT_DB(db);

    ClientServerFixture fixture(server_dir, test_context);
    fixture.start();

    {
        WriteTransaction wt{db};
        wt.get_group().add_table_with_primary_key("class_table", type_Int, "_id");
        wt.commit();
    }

    std::atomic<int> entry = 0;
    Session::Config config;
    config.progress_handler = [&](uint_fast64_t, uint_fast64_t, uint_fast64_t, uint_fast64_t, uint_fast64_t, double,
                                  double, int64_t) {
        ++entry;
    };

    Session session = fixture.make_session(db, "/test", std::move(config));

    // Each step calls wait_for_upload_complete twice because upload completion
    // is fired before progress handlers, so we need another hop through the
    // event loop after upload completion to know that the handler has been called
    session.wait_for_upload_complete_or_client_stopped();
    session.wait_for_upload_complete_or_client_stopped();

    // Binding produces two notifications: one after receiving
    // the DOWNLOAD message, and one after uploading the schema
    CHECK_EQUAL(entry, 2);

    // No notification sent because an empty commit doesn't change uploadable_bytes
    {
        WriteTransaction wt{db};
        wt.commit();
    }
    session.wait_for_upload_complete_or_client_stopped();
    session.wait_for_upload_complete_or_client_stopped();
    CHECK_EQUAL(entry, 2);

    // Both the external and local commits are empty, so again no change in
    // uploadable_bytes
    {
        auto db2 = DB::create(make_client_replication(), db_path);
        WriteTransaction wt{db2};
        wt.commit();
        WriteTransaction wt2{db};
        wt2.commit();
    }
    session.wait_for_upload_complete_or_client_stopped();
    session.wait_for_upload_complete_or_client_stopped();
    CHECK_EQUAL(entry, 2);

    // Local commit is empty, but the changeset created by the external write
    // is discovered after the local write, resulting in two notifications (one
    // before uploading and one after).
    {
        auto db2 = DB::create(make_client_replication(), db_path);
        WriteTransaction wt{db2};
        wt.get_table("class_table")->create_object_with_primary_key(0);
        wt.commit();
        WriteTransaction wt2{db};
        wt2.commit();
    }
    session.wait_for_upload_complete_or_client_stopped();
    session.wait_for_upload_complete_or_client_stopped();
    CHECK_EQUAL(entry, 4);
}

TEST(Sync_MultipleSyncAgentsNotAllowed)
{
    // At most one sync agent is allowed to participate in a Barq file access
    // session at any particular point in time. Note that a Barq file access
    // session is a group of temporally overlapping accesses to a Barq file,
    // and that the group of participants is the transitive closure of a
    // particular session participant over the "temporally overlapping access"
    // relation.

    TEST_DIR(server_dir);
    TEST_CLIENT_DB(db);

    auto pf = util::make_promise_future();
    struct Observer : BindingCallbackThreadObserver {
        unit_test::TestContext& test_context;
        util::Promise<void>& got_error;
        Observer(unit_test::TestContext& test_context, util::Promise<void>& got_error)
            : test_context(test_context)
            , got_error(got_error)
        {
        }

        bool has_handle_error() override
        {
            return true;
        }
        bool handle_error(const std::exception& e) override
        {
            CHECK(dynamic_cast<const MultipleSyncAgents*>(&e));
            got_error.emplace_value();
            return true;
        }
    };

    auto observer = std::make_shared<Observer>(test_context, pf.promise);
    ClientServerFixture::Config config;
    config.socket_provider_observer = observer;
    ClientServerFixture fixture(server_dir, test_context, std::move(config));
    fixture.start();

    {
        Session session = fixture.make_session(db, "/test");
        Session session2 = fixture.make_session(db, "/test");
        pf.future.get();

        // The exception caused the event loop to stop so we need to restart it
        fixture.start_client(0);
    }

    // Verify that after the error occurs (and is ignored) things are still
    // in a functional state
    Session session = fixture.make_session(db, "/test");
    session.wait_for_upload_complete_or_client_stopped();
}

TEST(Sync_CancelReconnectDelay)
{
    TEST_DIR(server_dir);
    TEST_CLIENT_DB(db);
    TEST_CLIENT_DB(db_x);

    ClientServerFixture::Config fixture_config;
    fixture_config.one_connection_per_session = false;

    auto expect_status = [&](BowlOfStonesSemaphore& bowl, ErrorCodes::Error code) {
        Session::Config config;
        config.connection_state_change_listener = [&, code](ConnectionState state,
                                                            std::optional<SessionErrorInfo> error) {
            if (state != ConnectionState::disconnected)
                return;
            CHECK(error);
            if (CHECK_EQUAL(error->status, code))
                bowl.add_stone();
        };
        return config;
    };

    // After connection-level error, and at session-level.
    {
        ClientServerFixture fixture{server_dir, test_context, std::move(fixture_config)};
        fixture.start();

        BowlOfStonesSemaphore bowl;
        Session session = fixture.make_session(db, "/test", expect_status(bowl, ErrorCodes::ConnectionClosed));
        session.wait_for_download_complete_or_client_stopped();
        fixture.close_server_side_connections();
        bowl.get_stone();

        session.cancel_reconnect_delay();
        session.wait_for_download_complete_or_client_stopped();
    }

    // After connection-level error, and at client-level while connection
    // object exists (ConnectionImpl in clinet.cpp).
    {
        ClientServerFixture fixture{server_dir, test_context, std::move(fixture_config)};
        fixture.start();

        BowlOfStonesSemaphore bowl;
        Session session = fixture.make_session(db, "/test", expect_status(bowl, ErrorCodes::ConnectionClosed));
        session.wait_for_download_complete_or_client_stopped();
        fixture.close_server_side_connections();
        bowl.get_stone();

        fixture.cancel_reconnect_delay();
        session.wait_for_download_complete_or_client_stopped();
    }

    // After connection-level error, and at client-level while connection object
    // does not exist (ConnectionImpl in clinet.cpp).
    {
        ClientServerFixture fixture{server_dir, test_context, std::move(fixture_config)};
        fixture.start();

        {
            BowlOfStonesSemaphore bowl;
            Session session = fixture.make_session(db, "/test", expect_status(bowl, ErrorCodes::ConnectionClosed));
            session.wait_for_download_complete_or_client_stopped();
            fixture.close_server_side_connections();
            bowl.get_stone();
        }

        fixture.wait_for_session_terminations_or_client_stopped();
        fixture.wait_for_session_terminations_or_client_stopped();
        // The connection object no longer exists at this time. After the first
        // of the two waits above, the invocation of ConnectionImpl::on_idle()
        // (in client.cpp) has been scheduled. After the second wait, it has
        // been called, and that destroys the connection object.

        fixture.cancel_reconnect_delay();
        {
            Session session = fixture.make_bound_session(db, "/test");
            session.wait_for_download_complete_or_client_stopped();
        }
    }

    // After session-level error, and at session-level.
    {
        ClientServerFixture fixture{server_dir, test_context, std::move(fixture_config)};
        fixture.start();

        // Add a session for the purpose of keeping the connection open
        Session session_x = fixture.make_bound_session(db_x, "/x");
        session_x.wait_for_download_complete_or_client_stopped();

        BowlOfStonesSemaphore bowl;
        Session session = fixture.make_session(db, "/..", expect_status(bowl, ErrorCodes::BadSyncPartitionValue));
        bowl.get_stone();

        session.cancel_reconnect_delay();
        bowl.get_stone();
    }

    // After session-level error, and at client-level.
    {
        ClientServerFixture fixture{server_dir, test_context, std::move(fixture_config)};
        fixture.start();

        // Add a session for the purpose of keeping the connection open
        Session session_x = fixture.make_bound_session(db_x, "/x");
        session_x.wait_for_download_complete_or_client_stopped();

        BowlOfStonesSemaphore bowl;
        Session session = fixture.make_session(db, "/..", expect_status(bowl, ErrorCodes::BadSyncPartitionValue));
        bowl.get_stone();

        fixture.cancel_reconnect_delay();
        bowl.get_stone();
    }
}


#ifndef BARQ_PLATFORM_WIN32

// This test checks that it is possible to create, upload, download, and merge
// changesets larger than 16 MB.
//
// Fails with 'bad alloc' around 1 GB mem usage on 32-bit Windows + 32-bit Linux
TEST_IF(Sync_MergeLargeBinary, !(BARQ_ARCHITECTURE_X86_32))
{
    // Two binaries are inserted in each transaction such that the total size
    // of the changeset exceeds 16 MB. A single set_binary operation does not
    // accept a binary larger than 16 MB.
    size_t binary_sizes[] = {
        static_cast<size_t>(8e6), static_cast<size_t>(9e6),  static_cast<size_t>(7e6), static_cast<size_t>(11e6),
        static_cast<size_t>(6e6), static_cast<size_t>(12e6), static_cast<size_t>(5e6), static_cast<size_t>(13e6),
    };

    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    {
        WriteTransaction wt(db_1);
        TableRef table = wt.get_group().add_table_with_primary_key("class_table name", type_Int, "id");
        table->add_column(type_Binary, "column name");
        std::string str_1(binary_sizes[0], 'a');
        BinaryData bd_1(str_1.data(), str_1.size());
        std::string str_2(binary_sizes[1], 'b');
        BinaryData bd_2(str_2.data(), str_2.size());
        table->create_object_with_primary_key(1).set("column name", bd_1);
        table->create_object_with_primary_key(2).set("column name", bd_2);
        wt.commit();
    }

    {
        WriteTransaction wt(db_1);
        TableRef table = wt.get_table("class_table name");
        std::string str_1(binary_sizes[2], 'c');
        BinaryData bd_1(str_1.data(), str_1.size());
        std::string str_2(binary_sizes[3], 'd');
        BinaryData bd_2(str_2.data(), str_2.size());
        table->create_object_with_primary_key(3).set("column name", bd_1);
        table->create_object_with_primary_key(4).set("column name", bd_2);
        wt.commit();
    }

    {
        WriteTransaction wt(db_2);
        TableRef table = wt.get_group().add_table_with_primary_key("class_table name", type_Int, "id");
        table->add_column(type_Binary, "column name");
        std::string str_1(binary_sizes[4], 'e');
        BinaryData bd_1(str_1.data(), str_1.size());
        std::string str_2(binary_sizes[5], 'f');
        BinaryData bd_2(str_2.data(), str_2.size());
        table->create_object_with_primary_key(5).set("column name", bd_1);
        table->create_object_with_primary_key(6).set("column name", bd_2);
        wt.commit();
    }

    {
        WriteTransaction wt(db_2);
        TableRef table = wt.get_table("class_table name");
        std::string str_1(binary_sizes[6], 'g');
        BinaryData bd_1(str_1.data(), str_1.size());
        std::string str_2(binary_sizes[7], 'h');
        BinaryData bd_2(str_2.data(), str_2.size());
        table->create_object_with_primary_key(7).set("column name", bd_1);
        table->create_object_with_primary_key(8).set("column name", bd_2);
        wt.commit();
    }

    std::uint_fast64_t downloaded_bytes_1 = 0;
    std::uint_fast64_t downloadable_bytes_1 = 0;
    std::uint_fast64_t uploaded_bytes_1 = 0;
    std::uint_fast64_t uploadable_bytes_1 = 0;

    auto progress_handler_1 = [&](std::uint_fast64_t downloaded_bytes, std::uint_fast64_t downloadable_bytes,
                                  std::uint_fast64_t uploaded_bytes, std::uint_fast64_t uploadable_bytes,
                                  std::uint_fast64_t, double, double, int64_t) {
        downloaded_bytes_1 = downloaded_bytes;
        downloadable_bytes_1 = downloadable_bytes;
        uploaded_bytes_1 = uploaded_bytes;
        uploadable_bytes_1 = uploadable_bytes;
    };

    std::uint_fast64_t downloaded_bytes_2 = 0;
    std::uint_fast64_t downloadable_bytes_2 = 0;
    std::uint_fast64_t uploaded_bytes_2 = 0;
    std::uint_fast64_t uploadable_bytes_2 = 0;

    auto progress_handler_2 = [&](uint_fast64_t downloaded_bytes, uint_fast64_t downloadable_bytes,
                                  uint_fast64_t uploaded_bytes, uint_fast64_t uploadable_bytes, uint_fast64_t, double,
                                  double, int64_t) {
        downloaded_bytes_2 = downloaded_bytes;
        downloadable_bytes_2 = downloadable_bytes;
        uploaded_bytes_2 = uploaded_bytes;
        uploadable_bytes_2 = uploadable_bytes;
    };

    {
        TEST_DIR(dir);
        MultiClientServerFixture fixture(2, 1, dir, test_context);
        fixture.start();

        {
            Session::Config config;
            config.progress_handler = progress_handler_1;
            Session session_1 = fixture.make_session(0, 0, db_1, "/test", std::move(config));
            session_1.wait_for_upload_complete_or_client_stopped();
        }

        {
            Session::Config config;
            config.progress_handler = progress_handler_2;
            Session session_2 = fixture.make_session(1, 0, db_2, "/test", std::move(config));
            session_2.wait_for_download_complete_or_client_stopped();
            session_2.wait_for_upload_complete_or_client_stopped();
        }

        {
            Session::Config config;
            config.progress_handler = progress_handler_1;
            Session session_1 = fixture.make_session(0, 0, db_1, "/test", std::move(config));
            session_1.wait_for_download_complete_or_client_stopped();
        }
    }

    ReadTransaction read_1(db_1);
    ReadTransaction read_2(db_2);

    const Group& group = read_1;
    CHECK(compare_groups(read_1, read_2));
    ConstTableRef table = group.get_table("class_table name");
    CHECK_EQUAL(table->size(), 8);
    {
        const Obj obj = *table->begin();
        ChunkedBinaryData cb{obj.get<BinaryData>("column name")};
        CHECK((cb.size() == binary_sizes[0] && cb[0] == 'a') || (cb.size() == binary_sizes[4] && cb[0] == 'e'));
    }
    {
        const Obj obj = *(table->begin() + 7);
        ChunkedBinaryData cb{obj.get<BinaryData>("column name")};
        CHECK((cb.size() == binary_sizes[3] && cb[0] == 'd') || (cb.size() == binary_sizes[7] && cb[0] == 'h'));
    }

    CHECK_EQUAL(downloadable_bytes_1, downloaded_bytes_1);
    CHECK_EQUAL(uploadable_bytes_1, uploaded_bytes_1);
    CHECK_NOT_EQUAL(uploaded_bytes_1, 0);

    CHECK_EQUAL(downloadable_bytes_2, downloaded_bytes_2);
    CHECK_EQUAL(uploadable_bytes_2, uploaded_bytes_2);
    CHECK_NOT_EQUAL(uploaded_bytes_2, 0);

    CHECK_EQUAL(uploaded_bytes_1, downloaded_bytes_2);
    CHECK_NOT_EQUAL(downloaded_bytes_1, 0);
}


// This test checks that it is possible to create, upload, download, and merge
// changesets larger than 16 MB. This test uses less memory than
// Sync_MergeLargeBinary.
TEST(Sync_MergeLargeBinaryReducedMemory)
{
    // Two binaries are inserted in a transaction such that the total size
    // of the changeset exceeds 16MB. A single set_binary operation does not
    // accept a binary larger than 16MB. Only one changeset is larger than
    // 16 MB in this test.
    size_t binary_sizes[] = {
        static_cast<size_t>(8e6), static_cast<size_t>(9e6),  static_cast<size_t>(7e4), static_cast<size_t>(11e4),
        static_cast<size_t>(6e4), static_cast<size_t>(12e4), static_cast<size_t>(5e4), static_cast<size_t>(13e4),
    };

    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    {
        WriteTransaction wt(db_1);
        TableRef table = wt.get_group().add_table_with_primary_key("class_table name", type_Int, "id");
        table->add_column(type_Binary, "column name");
        std::string str_1(binary_sizes[0], 'a');
        BinaryData bd_1(str_1.data(), str_1.size());
        std::string str_2(binary_sizes[1], 'b');
        BinaryData bd_2(str_2.data(), str_2.size());
        table->create_object_with_primary_key(1).set("column name", bd_1);
        table->create_object_with_primary_key(2).set("column name", bd_2);
        wt.commit();
    }

    {
        WriteTransaction wt(db_1);
        TableRef table = wt.get_table("class_table name");
        std::string str_1(binary_sizes[2], 'c');
        BinaryData bd_1(str_1.data(), str_1.size());
        std::string str_2(binary_sizes[3], 'd');
        BinaryData bd_2(str_2.data(), str_2.size());
        table->create_object_with_primary_key(3).set("column name", bd_1);
        table->create_object_with_primary_key(4).set("column name", bd_2);
        wt.commit();
    }

    {
        WriteTransaction wt(db_2);
        TableRef table = wt.get_group().add_table_with_primary_key("class_table name", type_Int, "id");
        table->add_column(type_Binary, "column name");
        std::string str_1(binary_sizes[4], 'e');
        BinaryData bd_1(str_1.data(), str_1.size());
        std::string str_2(binary_sizes[5], 'f');
        BinaryData bd_2(str_2.data(), str_2.size());
        table->create_object_with_primary_key(5).set("column name", bd_1);
        table->create_object_with_primary_key(6).set("column name", bd_2);
        wt.commit();
    }

    {
        WriteTransaction wt(db_2);
        TableRef table = wt.get_table("class_table name");
        std::string str_1(binary_sizes[6], 'g');
        BinaryData bd_1(str_1.data(), str_1.size());
        std::string str_2(binary_sizes[7], 'h');
        BinaryData bd_2(str_2.data(), str_2.size());
        table->create_object_with_primary_key(7).set("column name", bd_1);
        table->create_object_with_primary_key(8).set("column name", bd_2);
        wt.commit();
    }

    uint_fast64_t downloaded_bytes_1 = 0;
    uint_fast64_t downloadable_bytes_1 = 0;
    uint_fast64_t uploaded_bytes_1 = 0;
    uint_fast64_t uploadable_bytes_1 = 0;

    auto progress_handler_1 = [&](uint_fast64_t downloaded_bytes, uint_fast64_t downloadable_bytes,
                                  uint_fast64_t uploaded_bytes, uint_fast64_t uploadable_bytes,
                                  uint_fast64_t /* snapshot_version */, double, double, int64_t) {
        downloaded_bytes_1 = downloaded_bytes;
        downloadable_bytes_1 = downloadable_bytes;
        uploaded_bytes_1 = uploaded_bytes;
        uploadable_bytes_1 = uploadable_bytes;
    };

    uint_fast64_t downloaded_bytes_2 = 0;
    uint_fast64_t downloadable_bytes_2 = 0;
    uint_fast64_t uploaded_bytes_2 = 0;
    uint_fast64_t uploadable_bytes_2 = 0;

    auto progress_handler_2 = [&](uint_fast64_t downloaded_bytes, uint_fast64_t downloadable_bytes,
                                  uint_fast64_t uploaded_bytes, uint_fast64_t uploadable_bytes,
                                  uint_fast64_t /* snapshot_version */, double, double, int64_t) {
        downloaded_bytes_2 = downloaded_bytes;
        downloadable_bytes_2 = downloadable_bytes;
        uploaded_bytes_2 = uploaded_bytes;
        uploadable_bytes_2 = uploadable_bytes;
    };

    {
        TEST_DIR(dir);
        MultiClientServerFixture fixture(2, 1, dir, test_context);
        fixture.start();

        {
            Session::Config config;
            config.progress_handler = progress_handler_1;
            Session session_1 = fixture.make_session(0, 0, db_1, "/test", std::move(config));
            session_1.wait_for_upload_complete_or_client_stopped();
        }

        {
            Session::Config config;
            config.progress_handler = progress_handler_2;
            Session session_2 = fixture.make_session(1, 0, db_2, "/test", std::move(config));
            session_2.wait_for_download_complete_or_client_stopped();
            session_2.wait_for_upload_complete_or_client_stopped();
        }

        {
            Session::Config config;
            config.progress_handler = progress_handler_1;
            Session session_1 = fixture.make_session(0, 0, db_1, "/test", std::move(config));
            session_1.wait_for_download_complete_or_client_stopped();
        }
    }

    ReadTransaction read_1(db_1);
    ReadTransaction read_2(db_2);

    const Group& group = read_1;
    CHECK(compare_groups(read_1, read_2));
    ConstTableRef table = group.get_table("class_table name");
    CHECK_EQUAL(table->size(), 8);
    {
        const Obj obj = *table->begin();
        ChunkedBinaryData cb(obj.get<BinaryData>("column name"));
        CHECK((cb.size() == binary_sizes[0] && cb[0] == 'a') || (cb.size() == binary_sizes[4] && cb[0] == 'e'));
    }
    {
        const Obj obj = *(table->begin() + 7);
        ChunkedBinaryData cb(obj.get<BinaryData>("column name"));
        CHECK((cb.size() == binary_sizes[3] && cb[0] == 'd') || (cb.size() == binary_sizes[7] && cb[0] == 'h'));
    }

    CHECK_EQUAL(downloadable_bytes_1, downloaded_bytes_1);
    CHECK_EQUAL(uploadable_bytes_1, uploaded_bytes_1);
    CHECK_NOT_EQUAL(uploaded_bytes_1, 0);

    CHECK_EQUAL(downloadable_bytes_2, downloaded_bytes_2);
    CHECK_EQUAL(uploadable_bytes_2, uploaded_bytes_2);
    CHECK_NOT_EQUAL(uploaded_bytes_2, 0);

    CHECK_EQUAL(uploaded_bytes_1, downloaded_bytes_2);
    CHECK_NOT_EQUAL(downloaded_bytes_1, 0);
}


// This test checks that it is possible to create, upload, download, and merge
// changesets larger than 16MB.
TEST(Sync_MergeLargeChangesets)
{
    constexpr int number_of_rows = 200;

    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    {
        WriteTransaction wt(db_1);
        TableRef table = wt.get_group().add_table_with_primary_key("class_table name", type_Int, "id");
        table->add_column(type_Binary, "column name");
        table->add_column(type_Int, "integer column");
        wt.commit();
    }

    {
        WriteTransaction wt(db_2);
        TableRef table = wt.get_group().add_table_with_primary_key("class_table name", type_Int, "id");
        table->add_column(type_Binary, "column name");
        table->add_column(type_Int, "integer column");
        wt.commit();
    }

    {
        WriteTransaction wt(db_1);
        TableRef table = wt.get_table("class_table name");
        for (int i = 0; i < number_of_rows; ++i) {
            table->create_object_with_primary_key(i);
        }
        std::string str(100000, 'a');
        BinaryData bd(str.data(), str.size());
        for (int row = 0; row < number_of_rows; ++row) {
            table->get_object(size_t(row)).set("column name", bd);
            table->get_object(size_t(row)).set("integer column", 2 * row);
        }
        wt.commit();
    }

    {
        WriteTransaction wt(db_2);
        TableRef table = wt.get_table("class_table name");
        for (int i = 0; i < number_of_rows; ++i) {
            table->create_object_with_primary_key(i + number_of_rows);
        }
        std::string str(100000, 'b');
        BinaryData bd(str.data(), str.size());
        for (int row = 0; row < number_of_rows; ++row) {
            table->get_object(size_t(row)).set("column name", bd);
            table->get_object(size_t(row)).set("integer column", 2 * row + 1);
        }
        wt.commit();
    }

    {
        TEST_DIR(dir);
        MultiClientServerFixture fixture(2, 1, dir, test_context);

        Session session_1 = fixture.make_session(0, 0, db_1, "/test");
        Session session_2 = fixture.make_session(1, 0, db_2, "/test");

        fixture.start();

        session_1.wait_for_upload_complete_or_client_stopped();
        session_2.wait_for_upload_complete_or_client_stopped();
        session_1.wait_for_download_complete_or_client_stopped();
        session_2.wait_for_download_complete_or_client_stopped();
    }

    ReadTransaction read_1(db_1);
    ReadTransaction read_2(db_2);
    const Group& group = read_1;
    CHECK(compare_groups(read_1, read_2));
    ConstTableRef table = group.get_table("class_table name");
    CHECK_EQUAL(table->size(), 2 * number_of_rows);
}


TEST(Sync_MergeMultipleChangesets)
{
    constexpr int number_of_changesets = 100;
    constexpr int number_of_instructions = 10;

    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    std::atomic<int> id = 0;

    {
        WriteTransaction wt(db_1);
        TableRef table = wt.get_group().add_table_with_primary_key("class_table name", type_Int, "id");
        table->add_column(type_Int, "integer column");
        wt.commit();
    }

    {
        WriteTransaction wt(db_2);
        TableRef table = wt.get_group().add_table_with_primary_key("class_table name", type_Int, "id");
        table->add_column(type_Int, "integer column");
        wt.commit();
    }

    {
        for (int i = 0; i < number_of_changesets; ++i) {
            WriteTransaction wt(db_1);
            TableRef table = wt.get_table("class_table name");
            for (int j = 0; j < number_of_instructions; ++j) {
                auto obj = table->create_object_with_primary_key(id.fetch_add(1));
                obj.set("integer column", 2 * j);
            }
            wt.commit();
        }
    }

    {
        for (int i = 0; i < number_of_changesets; ++i) {
            WriteTransaction wt(db_2);
            TableRef table = wt.get_table("class_table name");
            for (int j = 0; j < number_of_instructions; ++j) {
                auto obj = table->create_object_with_primary_key(id.fetch_add(1));
                obj.set("integer column", 2 * j + 1);
            }
            wt.commit();
        }
    }

    {
        TEST_DIR(dir);
        MultiClientServerFixture fixture(2, 1, dir, test_context);


        // Start server and upload changes of first client.
        Session session_1 = fixture.make_session(0, 0, db_1, "/test");
        Session session_2 = fixture.make_session(1, 0, db_2, "/test");

        fixture.start_server(0);
        fixture.start_client(0);
        session_1.wait_for_upload_complete_or_client_stopped();
        session_1.wait_for_download_complete_or_client_stopped();
        session_1.detach();
        // Stop first client.
        fixture.stop_client(0);

        // Start the second client and upload their changes.
        // Wait to integrate changes from the first client.
        fixture.start_client(1);
        session_2.wait_for_upload_complete_or_client_stopped();
        session_2.wait_for_download_complete_or_client_stopped();
    }

    ReadTransaction read_1(db_1);
    ReadTransaction read_2(db_2);
    const Group& group1 = read_1;
    const Group& group2 = read_2;
    ConstTableRef table1 = group1.get_table("class_table name");
    ConstTableRef table2 = group2.get_table("class_table name");
    CHECK_EQUAL(table1->size(), number_of_changesets * number_of_instructions);
    CHECK_EQUAL(table2->size(), 2 * number_of_changesets * number_of_instructions);
}


#endif // BARQ_PLATFORM_WIN32


TEST(Sync_PingTimesOut)
{
    bool did_fail = false;
    {
        TEST_DIR(dir);
        TEST_CLIENT_DB(db);

        ClientServerFixture::Config config;
        config.client_ping_period = 0;  // send ping immediately
        config.client_pong_timeout = 0; // time out immediately
        ClientServerFixture fixture(dir, test_context, std::move(config));

        auto error_handler = [&](Status status, bool) {
            CHECK_EQUAL(status, ErrorCodes::ConnectionClosed);
            CHECK_EQUAL(status.reason(), "Timed out waiting for PONG response from server");
            did_fail = true;
            fixture.stop();
        };
        fixture.set_client_side_error_handler(std::move(error_handler));

        fixture.start();

        Session session = fixture.make_bound_session(db);
        session.wait_for_download_complete_or_client_stopped();
    }
    CHECK(did_fail);
}


TEST(Sync_ReconnectAfterPingTimeout)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db);

    ClientServerFixture::Config config;
    config.client_ping_period = 0;  // send ping immediately
    config.client_pong_timeout = 0; // time out immediately

    ClientServerFixture fixture(dir, test_context, std::move(config));

    BowlOfStonesSemaphore bowl;
    auto error_handler = [&](Status status, bool) {
        if (CHECK_EQUAL(status, ErrorCodes::ConnectionClosed)) {
            CHECK_EQUAL(status.reason(), "Timed out waiting for PONG response from server");
            bowl.add_stone();
        }
    };
    fixture.set_client_side_error_handler(std::move(error_handler));
    fixture.start();

    Session session = fixture.make_bound_session(db, "/test");
    bowl.get_stone();
}


TEST(Sync_UrgentPingIsSent)
{
    bool did_fail = false;
    {
        TEST_DIR(dir);
        TEST_CLIENT_DB(db);

        ClientServerFixture::Config config;
        config.client_pong_timeout = 0; // urgent pings time out immediately

        ClientServerFixture fixture(dir, test_context, std::move(config));

        auto error_handler = [&](Status status, bool) {
            CHECK_EQUAL(status, ErrorCodes::ConnectionClosed);
            CHECK_EQUAL(status.reason(), "Timed out waiting for PONG response from server");
            did_fail = true;
            fixture.stop();
        };
        fixture.set_client_side_error_handler(std::move(error_handler));

        fixture.start();

        Session session = fixture.make_bound_session(db);
        session.wait_for_download_complete_or_client_stopped(); // ensure connection established
        session.cancel_reconnect_delay();                       // send an urgent ping
        session.wait_for_download_complete_or_client_stopped();
    }
    CHECK(did_fail);
}


TEST(Sync_ServerDiscardDeadConnections)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db);

    ClientServerFixture::Config config;
    config.server_connection_reaper_interval = 1; // discard dead connections quickly, FIXME: 0 will not work here :(

    ClientServerFixture fixture(dir, test_context, std::move(config));

    BowlOfStonesSemaphore bowl;
    auto error_handler = [&](Status status, bool) {
        CHECK_EQUAL(status, ErrorCodes::ConnectionClosed);
        bowl.add_stone();
    };
    fixture.set_client_side_error_handler(std::move(error_handler));
    fixture.start();

    Session session = fixture.make_bound_session(db);
    session.wait_for_download_complete_or_client_stopped(); // ensure connection established
    fixture.set_server_connection_reaper_timeout(0);        // all connections will now be considered dead
    bowl.get_stone();
}


TEST(Sync_Quadratic_Merge)
{
    size_t num_instructions_1 = 100;
    size_t num_instructions_2 = 200;
    BARQ_ASSERT(num_instructions_1 >= 3 && num_instructions_2 >= 3);

    TEST_DIR(server_dir);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    // The schema and data is created with
    // n_operations instructions. The instructions are:
    // create table
    // add column
    // create object
    // n_operations - 3 add_int instructions.
    auto create_data = [](DBRef db, size_t n_operations) {
        WriteTransaction wt(db);
        TableRef table = wt.get_group().add_table_with_primary_key("class_table", type_Int, "id");
        table->add_column(type_Int, "i");
        Obj obj = table->create_object_with_primary_key(1);
        for (size_t i = 0; i < n_operations - 3; ++i)
            obj.add_int("i", 1);
        wt.commit();
    };

    create_data(db_1, num_instructions_1);
    create_data(db_2, num_instructions_2);

    int num_clients = 2;
    int num_servers = 1;
    MultiClientServerFixture fixture{num_clients, num_servers, server_dir, test_context};
    fixture.start();

    Session session_1 = fixture.make_session(0, 0, db_1, "/test");
    session_1.wait_for_upload_complete_or_client_stopped();

    Session session_2 = fixture.make_session(1, 0, db_2, "/test");
    session_2.wait_for_upload_complete_or_client_stopped();

    session_1.wait_for_download_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();
}


TEST(Sync_BatchedUploadMessages)
{
    TEST_DIR(server_dir);
    TEST_CLIENT_DB(db);

    ClientServerFixture fixture(server_dir, test_context);
    fixture.start();

    {
        WriteTransaction wt{db};
        TableRef tr = wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
        tr->add_column(type_Int, "integer column");
        wt.commit();
    }

    // Create a lot of changesets. We will attempt to check that
    // they are uploaded in a few upload messages.
    for (int i = 0; i < 400; ++i) {
        WriteTransaction wt{db};
        TableRef tr = wt.get_table("class_foo");
        tr->create_object_with_primary_key(i).set("integer column", i);
        wt.commit();
    }

    Session::Config config;
    auto pf = util::make_promise_future();
    config.progress_handler = [&](uint_fast64_t downloaded_bytes, uint_fast64_t downloadable_bytes,
                                  uint_fast64_t uploaded_bytes, uint_fast64_t uploadable_bytes, uint_fast64_t, double,
                                  double, int64_t) {
        CHECK_GREATER(uploadable_bytes, 1000);

        // This is the important check. If the changesets were not batched,
        // there would be callbacks with partial uploaded_bytes.
        // With batching, all uploadable_bytes are uploaded in the same message.
        CHECK(uploaded_bytes == 0 || uploaded_bytes == uploadable_bytes);
        CHECK_EQUAL(0, downloaded_bytes);
        CHECK_EQUAL(0, downloadable_bytes);
        if (uploaded_bytes == uploadable_bytes) {
            pf.promise.emplace_value();
        }
    };

    Session session = fixture.make_session(db, "/test", std::move(config));
    session.wait_for_upload_complete_or_client_stopped();
    pf.future.get();
}


TEST(Sync_UploadLogCompactionEnabled)
{
    TEST_DIR(server_dir);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    ClientServerFixture::Config config;
    config.disable_upload_compaction = false;
    ClientServerFixture fixture(server_dir, test_context, std::move(config));
    fixture.start();

    // Create a changeset with lots of overwrites of the
    // same fields.
    {
        WriteTransaction wt{db_1};
        TableRef tr = wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
        tr->add_column(type_Int, "integer column");
        Obj obj0 = tr->create_object_with_primary_key(0);
        Obj obj1 = tr->create_object_with_primary_key(1);
        for (int i = 0; i < 10000; ++i) {
            obj0.set("integer column", i);
            obj1.set("integer column", 2 * i);
        }
        wt.commit();
    }

    Session session_1 = fixture.make_session(db_1, "/test");
    session_1.wait_for_upload_complete_or_client_stopped();

    Session::Config session_config;
    session_config.progress_handler = [&](uint_fast64_t downloaded_bytes, uint_fast64_t downloadable_bytes,
                                          uint_fast64_t uploaded_bytes, uint_fast64_t uploadable_bytes,
                                          uint_fast64_t snapshot_version, double, double, int64_t) {
        CHECK_EQUAL(downloaded_bytes, downloadable_bytes);
        CHECK_EQUAL(0, uploaded_bytes);
        CHECK_EQUAL(0, uploadable_bytes);
        static_cast<void>(snapshot_version);
        CHECK_NOT_EQUAL(downloadable_bytes, 0);
    };

    Session session_2 = fixture.make_session(db_2, "/test", std::move(session_config));
    session_2.wait_for_download_complete_or_client_stopped();

    {
        ReadTransaction rt_1(db_1);
        ReadTransaction rt_2(db_2);
        CHECK(compare_groups(rt_1, rt_2, *test_context.logger));
        ConstTableRef table = rt_1.get_table("class_foo");
        CHECK_EQUAL(2, table->size());
        CHECK_EQUAL(9999, table->begin()->get<Int>("integer column"));
        CHECK_EQUAL(19998, table->get_object(1).get<Int>("integer column"));
    }
}


TEST(Sync_UploadLogCompactionDisabled)
{
    TEST_DIR(server_dir);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    ClientServerFixture::Config config;
    config.disable_upload_compaction = true;
    config.disable_history_compaction = true;
    ClientServerFixture fixture{server_dir, test_context, std::move(config)};
    fixture.start();

    // Create a changeset with lots of overwrites of the
    // same fields.
    {
        WriteTransaction wt{db_1};
        TableRef tr = wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
        auto col_int = tr->add_column(type_Int, "integer column");
        Obj obj0 = tr->create_object_with_primary_key(0);
        Obj obj1 = tr->create_object_with_primary_key(1);
        for (int i = 0; i < 10000; ++i) {
            obj0.set(col_int, i);
            obj1.set(col_int, 2 * i);
        }
        wt.commit();
    }

    Session session_1 = fixture.make_bound_session(db_1, "/test");
    session_1.wait_for_upload_complete_or_client_stopped();

    Session::Config session_config;
    session_config.progress_handler = [&](uint_fast64_t downloaded_bytes, uint_fast64_t downloadable_bytes,
                                          uint_fast64_t uploaded_bytes, uint_fast64_t uploadable_bytes,
                                          uint_fast64_t snapshot_version, double, double, int64_t) {
        CHECK_EQUAL(downloaded_bytes, downloadable_bytes);
        CHECK_EQUAL(0, uploaded_bytes);
        CHECK_EQUAL(0, uploadable_bytes);
        static_cast<void>(snapshot_version);
        CHECK_NOT_EQUAL(0, downloadable_bytes);
    };

    Session session_2 = fixture.make_session(db_2, "/test", std::move(session_config));
    session_2.wait_for_download_complete_or_client_stopped();

    {
        ReadTransaction rt_1(db_1);
        ReadTransaction rt_2(db_2);
        CHECK(compare_groups(rt_1, rt_2, *test_context.logger));
        ConstTableRef table = rt_1.get_table("class_foo");
        CHECK_EQUAL(2, table->size());
        CHECK_EQUAL(9999, table->begin()->get<Int>("integer column"));
        CHECK_EQUAL(19998, table->get_object(1).get<Int>("integer column"));
    }
}


TEST(Sync_ReadOnlyClientSideHistoryTrim)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    ClientServerFixture fixture{dir, test_context};
    fixture.start();

    ColKey col_ndx_blob_data;
    {
        WriteTransaction wt{db_1};
        TableRef blobs = wt.get_group().add_table_with_primary_key("class_Blob", type_Int, "id");
        col_ndx_blob_data = blobs->add_column(type_Binary, "data");
        blobs->create_object_with_primary_key(1);
        wt.commit();
    }

    Session session_1 = fixture.make_bound_session(db_1, "/foo");
    Session session_2 = fixture.make_bound_session(db_2, "/foo");

    std::string blob(0x4000, '\0');
    for (long i = 0; i < 1024; ++i) {
        {
            WriteTransaction wt{db_1};
            TableRef blobs = wt.get_table("class_Blob");
            blobs->begin()->set(col_ndx_blob_data, BinaryData{blob});
            wt.commit();
        }
        session_1.wait_for_upload_complete_or_client_stopped();
        session_2.wait_for_download_complete_or_client_stopped();
    }

    // Check that the file size is less than 4 MiB. If it is, then the history
    // must have been trimmed, as the combined size of all the blobs is at least
    // 16 MiB.
    CHECK_LESS(util::File{db_1_path}.get_size(), 0x400000);
}

// This test creates two objects in a target table and a link list
// in a source table. The first target object is inserted in the link list,
// and later the link is set to the second target object.
// Both the target objects are deleted afterwards. The tests verifies that
// sync works with log compaction turned on.
TEST(Sync_ContainerInsertAndSetLogCompaction)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);
    ClientServerFixture fixture(dir, test_context);
    fixture.start();

    {
        WriteTransaction wt{db_1};

        TableRef table_target = wt.get_group().add_table_with_primary_key("class_target", type_Int, "id");
        ColKey col_ndx = table_target->add_column(type_Int, "value");
        auto k0 = table_target->create_object_with_primary_key(1).set(col_ndx, 123).get_key();
        auto k1 = table_target->create_object_with_primary_key(2).set(col_ndx, 456).get_key();

        TableRef table_source = wt.get_group().add_table_with_primary_key("class_source", type_Int, "id");
        col_ndx = table_source->add_column_list(*table_target, "target_link");
        Obj obj = table_source->create_object_with_primary_key(1);
        LnkLst ll = obj.get_linklist(col_ndx);
        ll.insert(0, k0);
        ll.set(0, k1);

        table_target->remove_object(k1);
        table_target->remove_object(k0);

        wt.commit();
    }

    Session session_1 = fixture.make_bound_session(db_1);
    session_1.wait_for_upload_complete_or_client_stopped();

    Session session_2 = fixture.make_bound_session(db_2);
    session_2.wait_for_download_complete_or_client_stopped();

    {
        ReadTransaction rt_1(db_1);
        ReadTransaction rt_2(db_2);
        CHECK(compare_groups(rt_1, rt_2, *test_context.logger));
    }
}


TEST(Sync_MultipleContainerColumns)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);
    ClientServerFixture fixture(dir, test_context);
    fixture.start();

    {
        WriteTransaction wt{db_1};

        TableRef table = wt.get_group().add_table_with_primary_key("class_Table", type_Int, "id");
        table->add_column_list(type_String, "array1");
        table->add_column_list(type_String, "array2");

        Obj row = table->create_object_with_primary_key(1);
        {
            Lst<StringData> array1 = row.get_list<StringData>("array1");
            array1.clear();
            array1.add("Hello");
        }
        {
            Lst<StringData> array2 = row.get_list<StringData>("array2");
            array2.clear();
            array2.add("World");
        }

        wt.commit();
    }

    Session session_1 = fixture.make_bound_session(db_1);
    session_1.wait_for_upload_complete_or_client_stopped();

    Session session_2 = fixture.make_bound_session(db_2);
    session_2.wait_for_download_complete_or_client_stopped();

    {
        ReadTransaction rt_1(db_1);
        ReadTransaction rt_2(db_2);
        CHECK(compare_groups(rt_1, rt_2, *test_context.logger));

        ConstTableRef table = rt_1.get_table("class_Table");
        const Obj row = *table->begin();
        auto array1 = row.get_list<StringData>("array1");
        auto array2 = row.get_list<StringData>("array2");
        CHECK_EQUAL(array1.size(), 1);
        CHECK_EQUAL(array2.size(), 1);
        CHECK_EQUAL(array1.get(0), "Hello");
        CHECK_EQUAL(array2.get(0), "World");
    }
}


TEST(Sync_ConnectionStateChange)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    std::vector<ConnectionState> states_1, states_2;
    {
        ClientServerFixture fixture(dir, test_context);
        fixture.start();

        BowlOfStonesSemaphore bowl_1, bowl_2;
        auto listener_1 = [&](ConnectionState state, util::Optional<ErrorInfo> error_info) {
            CHECK_EQUAL(state == ConnectionState::disconnected, bool(error_info));
            states_1.push_back(state);
            if (state == ConnectionState::disconnected)
                bowl_1.add_stone();
        };
        auto listener_2 = [&](ConnectionState state, util::Optional<ErrorInfo> error_info) {
            CHECK_EQUAL(state == ConnectionState::disconnected, bool(error_info));
            states_2.push_back(state);
            if (state == ConnectionState::disconnected)
                bowl_2.add_stone();
        };

        Session::Config config_1;
        config_1.connection_state_change_listener = listener_1;
        Session session_1 = fixture.make_session(db_1, "/test", std::move(config_1));
        session_1.wait_for_download_complete_or_client_stopped();

        Session::Config config_2;
        config_2.connection_state_change_listener = listener_2;
        Session session_2 = fixture.make_session(db_2, "/test", std::move(config_2));
        session_2.wait_for_download_complete_or_client_stopped();

        fixture.close_server_side_connections();
        bowl_1.get_stone();
        bowl_2.get_stone();
    }
    std::vector<ConnectionState> reference{ConnectionState::connecting, ConnectionState::connected,
                                           ConnectionState::disconnected};
    CHECK(states_1 == reference);
    CHECK(states_2 == reference);
}


TEST(Sync_VerifyServerHistoryAfterLargeUpload)
{
    TEST_DIR(server_dir);
    TEST_CLIENT_DB(db);

    ClientServerFixture fixture{server_dir, test_context};
    fixture.start();

    {
        auto wt = db->start_write();
        auto table = wt->add_table_with_primary_key("class_table", type_Int, "id");
        ColKey col = table->add_column(type_Binary, "data");

        // Create enough data that our changeset cannot be stored contiguously
        // by BinaryColumn (> 16MB).
        std::size_t data_size = 8 * 1024 * 1024;
        std::string data(data_size, '\0');
        for (int i = 0; i < 8; ++i) {
            table->create_object_with_primary_key(i).set(col, BinaryData{data.data(), data.size()});
        }

        wt->commit();

        Session session = fixture.make_session(db, "/test");
        session.wait_for_upload_complete_or_client_stopped();
    }

    {
        std::string server_path = fixture.map_virtual_to_real_path("/test");
        TestServerHistoryContext context;
        _impl::ServerHistory history{context};
        DBRef db = DB::create(history, server_path);
        {
            ReadTransaction rt{db};
            rt.get_group().verify();
        }
    }
}


TEST(Sync_ServerSideModify_Randomize)
{
    int num_server_side_transacts = 1200;
    int num_client_side_transacts = 1200;

    TEST_DIR(server_dir);
    TEST_CLIENT_DB(db_2);

    ClientServerFixture::Config config;
    ClientServerFixture fixture{server_dir, test_context, std::move(config)};
    fixture.start();

    Session session = fixture.make_bound_session(db_2, "/test");

    std::string server_path = fixture.map_virtual_to_real_path("/test");
    TestServerHistoryContext context;
    _impl::ServerHistory history_1{context};
    DBRef db_1 = DB::create(history_1, server_path);

    auto server_side_program = [num_server_side_transacts, &db_1, &fixture, &session] {
        Random random(random_int<unsigned long>()); // Seed from slow global generator
        for (int i = 0; i < num_server_side_transacts; ++i) {
            WriteTransaction wt{db_1};
            TableRef table = wt.get_table("class_foo");
            if (!table) {
                table = wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
                table->add_column(type_Int, "i");
            }
            if (i % 2 == 0)
                table->create_object_with_primary_key(0 - i);
            Obj obj = *(table->begin() + random.draw_int_mod(table->size()));
            obj.set<int64_t>("i", random.draw_int_max(0x0'7FFF'FFFF'FFFF'FFFF));
            wt.commit();
            fixture.inform_server_about_external_change("/test");
            session.wait_for_download_complete_or_client_stopped();
        }
    };

    auto client_side_program = [num_client_side_transacts, &db_2, &session] {
        Random random(random_int<unsigned long>()); // Seed from slow global generator
        for (int i = 0; i < num_client_side_transacts; ++i) {
            WriteTransaction wt{db_2};
            TableRef table = wt.get_table("class_foo");
            if (!table) {
                table = wt.get_group().add_table_with_primary_key("class_foo", type_Int, "id");
                table->add_column(type_Int, "i");
            }
            if (i % 2 == 0)
                table->create_object_with_primary_key(i);
            ;
            Obj obj = *(table->begin() + random.draw_int_mod(table->size()));
            obj.set<int64_t>("i", random.draw_int_max(0x0'7FFF'FFFF'FFFF'FFFF));
            wt.commit();
            if (i % 16 == 0)
                session.wait_for_upload_complete_or_client_stopped();
        }
    };

    ThreadWrapper server_program_thread;
    server_program_thread.start(std::move(server_side_program));
    client_side_program();
    CHECK(!server_program_thread.join());

    session.wait_for_upload_complete_or_client_stopped();
    session.wait_for_download_complete_or_client_stopped();

    ReadTransaction rt_1{db_1};
    ReadTransaction rt_2{db_2};
    CHECK(compare_groups(rt_1, rt_2, *test_context.logger));
}


// This test connects a sync client to the barq cloud service using a SSL
// connection. The purpose of the test is to check that the server's SSL
// certificate is accepted by the client.  The client will connect with an
// invalid token and get an error code back.  The check is that the error is
// not rejected certificate.  The test should be disabled under normal
// circumstances since it requires network access and cloud availability. The
// test might be enabled during testing of SSL functionality.
TEST_IF(Sync_SSL_Certificates, false)
{
    TEST_CLIENT_DB(db);

    const char* server_address[] = {
        "morten-krogh.us1.cloud.barq.io",
        "fantastic-cotton-shoes.us1.cloud.barq.io",
        "www.barq.io",
        "www.yahoo.com",
        "www.nytimes.com",
        "www.ibm.com",
        "www.ssllabs.com",
    };

    size_t num_servers = sizeof(server_address) / sizeof(server_address[0]);

    auto client_logger = std::make_shared<util::PrefixLogger>("Client: ", test_context.logger);

    for (size_t i = 0; i < num_servers; ++i) {
        Client::Config client_config;
        client_config.logger = client_logger;
        client_config.reconnect_mode = ReconnectMode::testing;
        Client client(client_config);

        Session::Config session_config;
        session_config.server_address = server_address[i];
        session_config.server_port = 443;
        session_config.barq_identifier = "/anything";
        session_config.protocol_envelope = ProtocolEnvelope::barqs;

        // Invalid token for the cloud.
        session_config.signed_user_token = g_signed_test_user_token;

        session_config.connection_state_change_listener = [&](ConnectionState state,
                                                              const util::Optional<ErrorInfo>& error_info) {
            if (state == ConnectionState::disconnected) {
                CHECK(error_info);
                client_logger->debug("State change: disconnected, error_code = %1, is_fatal = %2", error_info->status,
                                     error_info->is_fatal);
                // We expect to get through the SSL handshake but will hit an error due to the wrong token.
                CHECK_NOT_EQUAL(error_info->status, ErrorCodes::TlsHandshakeFailed);
                client.shutdown();
            }
        };

        Session session{client, db, nullptr, nullptr, std::move(session_config)};
        session.wait_for_download_complete_or_client_stopped();
    }
}


// Testing the custom authorization header name.  The sync protocol does not
// currently use the HTTP Authorization header, so the test is to watch the
// logs and see that the client use the right header name. Proxies and the sync
// server HTTP api use the Authorization header.
TEST(Sync_AuthorizationHeaderName)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db);

    const char* authorization_header_name = "X-Alternative-Name";
    ClientServerFixture::Config config;
    config.authorization_header_name = authorization_header_name;
    ClientServerFixture fixture(dir, test_context, std::move(config));
    fixture.start();

    Session::Config session_config;
    session_config.authorization_header_name = authorization_header_name;

    std::map<std::string, std::string> custom_http_headers;
    custom_http_headers["Header-Name-1"] = "Header-Value-1";
    custom_http_headers["Header-Name-2"] = "Header-Value-2";
    session_config.custom_http_headers = std::move(custom_http_headers);
    Session session = fixture.make_session(db, "/test", std::move(session_config));

    session.wait_for_download_complete_or_client_stopped();
}


TEST(Sync_BadChangeset)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db);

    bool did_fail = false;
    {
        ClientServerFixture::Config config;
        config.disable_upload_compaction = true;
        ClientServerFixture fixture(dir, test_context, std::move(config));
        fixture.start();

        {
            Session session = fixture.make_bound_session(db);
            session.wait_for_download_complete_or_client_stopped();
        }

        {
            WriteTransaction wt(db);
            TableRef table = wt.get_group().add_table_with_primary_key("class_Foo", type_Int, "id");
            table->add_column(type_Int, "i");
            table->create_object_with_primary_key(5).set_all(123);
            const ChangesetEncoder::Buffer& buffer = get_replication(db).get_instruction_encoder().buffer();
            char bad_instruction = 0x3e;
            const_cast<ChangesetEncoder::Buffer&>(buffer).append(&bad_instruction, 1);
            wt.commit();
        }

        Session::Config session_config;
        session_config.connection_state_change_listener = [&](ConnectionState state,
                                                              const util::Optional<ErrorInfo>& error_info) {
            if (state != ConnectionState::disconnected)
                return;
            BARQ_ASSERT(error_info);
            CHECK_EQUAL(error_info->status, ErrorCodes::BadChangeset);
            CHECK(error_info->is_fatal);
            did_fail = true;
            fixture.stop();
        };
        Session session = fixture.make_session(db, "/test", std::move(session_config));
        session.wait_for_upload_complete_or_client_stopped();
        session.wait_for_download_complete_or_client_stopped();
    }
    CHECK(did_fail);
}


TEST(Sync_GoodChangeset_AccentCharacterInFieldName)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db);

    bool did_fail = false;
    {
        ClientServerFixture::Config config;
        config.disable_upload_compaction = true;
        ClientServerFixture fixture(dir, test_context, std::move(config));
        fixture.start();

        {
            Session session = fixture.make_bound_session(db);
        }

        {
            WriteTransaction wt(db);
            TableRef table = wt.get_group().add_table_with_primary_key("class_table", type_Int, "id");
            table->add_column(type_Int, "prógram");
            table->add_column(type_Int, "program");
            auto obj = table->create_object_with_primary_key(1);
            obj.add_int("program", 42);
            wt.commit();
        }

        Session::Config session_config;
        session_config.connection_state_change_listener = [&](ConnectionState state,
                                                              const util::Optional<ErrorInfo>) {
            if (state != ConnectionState::disconnected)
                return;
            did_fail = true;
            fixture.stop();
        };
        Session session = fixture.make_session(db, "/test", std::move(session_config));
        session.wait_for_upload_complete_or_client_stopped();
    }
    CHECK_NOT(did_fail);
}


namespace issue2104 {

class ServerHistoryContext : public _impl::ServerHistory::Context {
public:
    ServerHistoryContext() {}

    std::mt19937_64& server_history_get_random() noexcept override
    {
        return m_random;
    }

private:
    std::mt19937_64 m_random;
};

} // namespace issue2104

// This test reproduces a slow merge seen in issue 2104.
// The test uses a user supplied Barq and a changeset
// from a client.
// The test uses a user supplied Barq that is very large
// and not kept in the repo. The barq has checksum 3693867489.
//
// This test might be modified to avoid having a large Barq
// (96 MB uncompressed) in the repo.
TEST_IF(Sync_Issue2104, false)
{
    TEST_DIR(dir);

    // Save a snapshot of the server Barq file.
    std::string barq_path = "issue_2104_server.barq";
    std::string barq_path_copy = util::File::resolve("issue_2104.barq", dir);
    util::File::copy(barq_path, barq_path_copy);

    std::string changeset_hex = "3F 00 07 41 42 43 44 61 74 61 3F 01 02 69 64 3F 02 09 41 6C 69 67 6E 6D 65 6E 74 3F "
                                "03 12 42 65 68 61 76 69 6F 72 4F 63 63 75 72 72 65 6E 63 65 3F 04 0D 42 65 68 61 76 "
                                "69 6F 72 50 68 61 73 65 3F 05 09 43 6F 6C 6C 65 63 74 6F 72 3F 06 09 43 72 69 74 65 "
                                "72 69 6F 6E 3F 07 07 46 65 61 74 75 72 65 3F 08 12 49 6E 73 74 72 75 63 74 69 6F 6E "
                                "61 6C 54 72 69 61 6C 3F 09 14 4D 65 61 73 75 72 65 6D 65 6E 74 50 72 6F 63 65 64 75 "
                                "72 65 3F 0A 07 4D 65 73 73 61 67 65 3F 0B 04 4E 6F 74 65 3F 0C 16 4F 6E 62 6F 61 72 "
                                "64 69 6E 67 54 6F 75 72 50 72 6F 67 72 65 73 73 3F 0D 05 50 68 61 73 65 3F 0E 07 50 "
                                "72 6F 67 72 61 6D 3F 0F 0C 50 72 6F 67 72 61 6D 47 72 6F 75 70 3F 10 0A 50 72 6F 67 "
                                "72 61 6D 52 75 6E 3F 11 0F 50 72 6F 67 72 61 6D 54 65 6D 70 6C 61 74 65 3F 12 0B 52 "
                                "65 61 6C 6D 53 74 72 69 6E 67 3F 13 0B 53 65 73 73 69 6F 6E 4E 6F 74 65 3F 14 07 53 "
                                "74 75 64 65 6E 74 3F 15 06 54 61 72 67 65 74 3F 16 0E 54 61 72 67 65 74 54 65 6D 70 "
                                "6C 61 74 65 3F 17 04 54 61 73 6B 3F 18 05 54 6F 6B 65 6E 3F 19 04 55 73 65 72 3F 1A "
                                "07 5F 5F 43 6C 61 73 73 3F 1B 04 6E 61 6D 65 3F 1C 0C 5F 5F 50 65 72 6D 69 73 73 69 "
                                "6F 6E 3F 1D 07 5F 5F 52 65 61 6C 6D 3F 1E 06 5F 5F 52 6F 6C 65 3F 1F 06 5F 5F 55 73 "
                                "65 72 3F 20 09 63 72 65 61 74 65 64 41 74 3F 21 0A 6D 6F 64 69 66 69 65 64 41 74 3F "
                                "22 09 63 72 65 61 74 65 64 42 79 3F 23 0A 6D 6F 64 69 66 69 65 64 42 79 3F 24 07 70 "
                                "72 6F 67 72 61 6D 3F 25 04 64 61 74 65 3F 26 0A 61 6E 74 65 63 65 64 65 6E 74 3F 27 "
                                "08 62 65 68 61 76 69 6F 72 3F 28 0B 63 6F 6E 73 65 71 75 65 6E 63 65 3F 29 07 73 65 "
                                "74 74 69 6E 67 3F 2A 04 6E 6F 74 65 3F 2B 08 63 61 74 65 67 6F 72 79 3F 2C 05 6C 65 "
                                "76 65 6C 3F 2D 0A 6F 63 63 75 72 72 65 64 41 74 3F 2E 05 70 68 61 73 65 3F 2F 08 64 "
                                "75 72 61 74 69 6F 6E 3F 30 07 6D 61 72 6B 52 61 77 3F 31 09 73 68 6F 72 74 4E 61 6D "
                                "65 3F 32 0A 64 65 66 69 6E 69 74 69 6F 6E 3F 33 06 74 61 72 67 65 74 3F 34 08 74 65 "
                                "6D 70 6C 61 74 65 3F 35 0D 6C 61 62 65 6C 4F 76 65 72 72 69 64 65 3F 36 08 62 61 73 "
                                "65 6C 69 6E 65 3F 37 13 63 6F 6C 6C 65 63 74 69 6F 6E 46 72 65 71 75 65 6E 63 79 3F "
                                "38 0E 61 64 64 69 74 69 6F 6E 61 6C 49 6E 66 6F 3F 39 0D 64 61 79 73 54 6F 49 6E 63 "
                                "6C 75 64 65 3F 3A 0D 64 61 79 73 54 6F 45 78 63 6C 75 64 65 3F 3B 07 74 79 70 65 52 "
                                "61 77 3F 3C 09 66 72 65 71 75 65 6E 63 79 3F 3D 08 69 6E 74 65 72 76 61 6C 3F 3E 0E "
                                "70 6F 69 6E 74 73 41 6E 61 6C 79 7A 65 64 3F 3F 0D 6D 69 6E 50 65 72 63 65 6E 74 61 "
                                "67 65 3F C0 00 04 63 6F 64 65 3F C1 00 06 74 65 61 6D 49 64 3F C2 00 03 75 72 6C 3F "
                                "C3 00 07 73 65 63 74 69 6F 6E 3F C4 00 11 63 72 69 74 65 72 69 6F 6E 44 65 66 61 75 "
                                "6C 74 73 3F C5 00 04 74 61 73 6B 3F C6 00 09 72 65 73 75 6C 74 52 61 77 3F C7 00 09 "
                                "70 72 6F 6D 70 74 52 61 77 3F C8 00 04 74 65 78 74 3F C9 00 0A 70 72 6F 67 72 61 6D "
                                "52 75 6E 3F CA 00 09 72 65 63 69 70 69 65 6E 74 3F CB 00 04 62 6F 64 79 3F CC 00 06 "
                                "61 63 74 69 76 65 3F CD 00 0D 62 65 68 61 76 69 6F 72 50 68 61 73 65 3F CE 00 03 64 "
                                "61 79 3F CF 00 06 74 6F 75 72 49 64 3F D0 00 08 63 6F 6D 70 6C 65 74 65 3F D1 00 05 "
                                "73 74 61 72 74 3F D2 00 03 65 6E 64 3F D3 00 05 74 69 74 6C 65 3F D4 00 12 70 72 6F "
                                "67 72 61 6D 44 65 73 63 72 69 70 74 69 6F 6E 3F D5 00 09 63 72 69 74 65 72 69 6F 6E "
                                "3F D6 00 0E 63 72 69 74 65 72 69 6F 6E 52 75 6C 65 73 3F D7 00 03 73 74 6F 3F D8 00 "
                                "03 6C 74 6F 3F D9 00 18 72 65 69 6E 66 6F 72 63 65 6D 65 6E 74 53 63 68 65 64 75 6C "
                                "65 52 61 77 3F DA 00 0D 72 65 69 6E 66 6F 72 63 65 6D 65 6E 74 3F DB 00 11 72 65 69 "
                                "6E 66 6F 72 63 65 6D 65 6E 74 54 79 70 65 3F DC 00 16 64 69 73 63 72 69 6D 69 6E 61 "
                                "74 69 76 65 53 74 69 6D 75 6C 75 73 3F DD 00 07 74 61 72 67 65 74 73 3F DE 00 05 74 "
                                "61 73 6B 73 3F DF 00 0A 74 61 73 6B 53 74 61 74 65 73 3F E0 00 0C 74 6F 74 61 6C 49 "
                                "54 43 6F 75 6E 74 3F E1 00 0A 73 61 6D 70 6C 65 54 69 6D 65 3F E2 00 10 64 65 66 61 "
                                "75 6C 74 52 65 73 75 6C 74 52 61 77 3F E3 00 0F 76 61 72 69 61 62 6C 65 49 54 43 6F "
                                "75 6E 74 3F E4 00 09 65 72 72 6F 72 6C 65 73 73 3F E5 00 0C 6D 69 6E 41 74 74 65 6D "
                                "70 74 65 64 3F E6 00 10 64 65 66 61 75 6C 74 4D 65 74 68 6F 64 52 61 77 3F E7 00 0A "
                                "73 65 74 74 69 6E 67 52 61 77 3F E8 00 07 73 74 75 64 65 6E 74 3F E9 00 0F 6D 61 73 "
                                "74 65 72 65 64 54 61 72 67 65 74 73 3F EA 00 0D 66 75 74 75 72 65 54 61 72 67 65 74 "
                                "73 3F EB 00 05 67 72 6F 75 70 3F EC 00 06 6C 6F 63 6B 65 64 3F ED 00 0E 6C 61 73 74 "
                                "44 65 63 69 73 69 6F 6E 41 74 3F EE 00 08 61 72 63 68 69 76 65 64 3F EF 00 0E 64 61 "
                                "74 65 73 54 6F 49 6E 63 6C 75 64 65 3F F0 00 0E 64 61 74 65 73 54 6F 45 78 63 6C 75 "
                                "64 65 3F F1 00 09 64 72 61 77 65 72 52 61 77 3F F2 00 0B 63 6F 6D 70 6C 65 74 65 64 "
                                "41 74 3F F3 00 03 49 54 73 3F F4 00 0C 64 69 73 70 6C 61 79 4F 72 64 65 72 3F F5 00 "
                                "0F 63 6F 72 72 65 63 74 4F 76 65 72 72 69 64 65 3F F6 00 11 61 74 74 65 6D 70 74 65 "
                                "64 4F 76 65 72 72 69 64 65 3F F7 00 09 6D 65 74 68 6F 64 52 61 77 3F F8 00 08 73 74 "
                                "61 74 65 52 61 77 3F F9 00 0C 70 6F 69 6E 74 54 79 70 65 52 61 77 3F FA 00 09 61 6C "
                                "69 67 6E 6D 65 6E 74 3F FB 00 08 65 78 61 6D 70 6C 65 73 3F FC 00 0E 67 65 6E 65 72 "
                                "61 6C 69 7A 61 74 69 6F 6E 3F FD 00 09 6D 61 74 65 72 69 61 6C 73 3F FE 00 09 6F 62 "
                                "6A 65 63 74 69 76 65 3F FF 00 0F 72 65 63 6F 6D 6D 65 6E 64 61 74 69 6F 6E 73 3F 80 "
                                "01 08 73 74 69 6D 75 6C 75 73 3F 81 01 0B 74 61 72 67 65 74 4E 6F 74 65 73 3F 82 01 "
                                "11 74 65 61 63 68 69 6E 67 50 72 6F 63 65 64 75 72 65 3F 83 01 0A 76 62 6D 61 70 70 "
                                "54 61 67 73 3F 84 01 08 61 66 6C 73 54 61 67 73 3F 85 01 09 6E 79 73 6C 73 54 61 67 "
                                "73 3F 86 01 06 64 6F 6D 61 69 6E 3F 87 01 04 67 6F 61 6C 3F 88 01 07 73 75 62 6A 65 "
                                "63 74 3F 89 01 0B 6A 6F 62 43 61 74 65 67 6F 72 79 3F 8A 01 13 70 72 6F 6D 70 74 69 "
                                "6E 67 50 72 6F 63 65 64 75 72 65 73 3F 8B 01 10 70 72 65 73 63 68 6F 6F 6C 4D 61 73 "
                                "74 65 72 79 3F 8C 01 0C 61 62 6C 6C 73 4D 61 73 74 65 72 79 3F 8D 01 0D 64 61 74 61 "
                                "52 65 63 6F 72 64 69 6E 67 3F 8E 01 0F 65 72 72 6F 72 43 6F 72 72 65 63 74 69 6F 6E "
                                "3F 8F 01 0B 73 74 72 69 6E 67 56 61 6C 75 65 3F 90 01 06 63 6C 69 65 6E 74 3F 91 01 "
                                "09 74 68 65 72 61 70 69 73 74 3F 92 01 0B 72 65 69 6E 66 6F 72 63 65 72 73 3F 93 01 "
                                "05 6E 6F 74 65 73 3F 94 01 0F 74 61 72 67 65 74 42 65 68 61 76 69 6F 72 73 3F 95 01 "
                                "08 67 6F 61 6C 73 4D 65 74 3F 96 01 0D 74 79 70 65 4F 66 53 65 72 76 69 63 65 3F 97 "
                                "01 0D 70 65 6F 70 6C 65 50 72 65 73 65 6E 74 3F 98 01 08 6C 61 74 69 74 75 64 65 3F "
                                "99 01 09 6C 6F 6E 67 69 74 75 64 65 3F 9A 01 06 61 6C 65 72 74 73 3F 9B 01 03 65 69 "
                                "6E 3F 9C 01 03 64 6F 62 3F 9D 01 0F 70 72 69 6D 61 72 79 47 75 61 72 64 69 61 6E 3F "
                                "9E 01 11 73 65 63 6F 6E 64 61 72 79 47 75 61 72 64 69 61 6E 3F 9F 01 08 69 6D 61 67 "
                                "65 55 72 6C 3F A0 01 0B 64 65 61 63 74 69 76 61 74 65 64 3F A1 01 11 74 61 72 67 65 "
                                "74 44 65 73 63 72 69 70 74 69 6F 6E 3F A2 01 08 6D 61 73 74 65 72 65 64 3F A3 01 0F "
                                "74 61 73 6B 44 65 73 63 72 69 70 74 69 6F 6E 3F A4 01 09 65 78 70 69 72 65 73 41 74 "
                                "3F A5 01 0C 63 6F 6C 6C 65 63 74 6F 72 49 64 73 3F A6 01 08 73 74 75 64 65 6E 74 73 "
                                "3F A7 01 12 6F 6E 62 6F 61 72 64 69 6E 67 50 72 6F 67 72 65 73 73 3F A8 01 05 65 6D "
                                "61 69 6C 3F A9 01 05 70 68 6F 6E 65 3F AA 01 07 72 6F 6C 65 52 61 77 3F AB 01 08 73 "
                                "65 74 74 69 6E 67 73 3F AC 01 0B 70 65 72 6D 69 73 73 69 6F 6E 73 3F AD 01 04 72 6F "
                                "6C 65 3F AE 01 07 63 61 6E 52 65 61 64 3F AF 01 09 63 61 6E 55 70 64 61 74 65 3F B0 "
                                "01 09 63 61 6E 44 65 6C 65 74 65 3F B1 01 11 63 61 6E 53 65 74 50 65 72 6D 69 73 73 "
                                "69 6F 6E 73 3F B2 01 08 63 61 6E 51 75 65 72 79 3F B3 01 09 63 61 6E 43 72 65 61 74 "
                                "65 3F B4 01 0F 63 61 6E 4D 6F 64 69 66 79 53 63 68 65 6D 61 3F B5 01 07 6D 65 6D 62 "
                                "65 72 73 02 00 01 01 02 00 02 02 01 01 02 00 02 03 01 01 02 00 02 04 01 01 02 00 02 "
                                "05 01 01 02 01 02 06 01 01 02 01 02 07 01 01 02 00 02 08 01 01 02 00 02 09 01 01 02 "
                                "00 02 0A 01 01 02 00 02 0B 01 01 02 00 02 0C 01 01 02 00 02 0D 01 01 02 00 02 0E 01 "
                                "01 02 00 02 0F 01 01 02 00 02 10 01 01 02 00 02 11 01 01 02 00 02 12 00 02 13 01 01 "
                                "02 00 02 14 01 01 02 00 02 15 01 01 02 00 02 16 01 01 02 00 02 17 01 01 02 00 02 18 "
                                "01 01 02 00 02 19 01 01 02 00 02 1A 01 1B 02 00 02 1C 00 02 1D 01 01 00 00 02 1E 01 "
                                "1B 02 00 02 1F 01 01 02 00 00 00 0B 20 08 00 00 0B 21 08 00 00 0B 22 0C 00 19 0B 23 "
                                "0C 00 19 0B 24 0C 00 0E 0B 25 08 00 00 0B 26 02 00 01 0B 27 02 00 01 0B 28 02 00 01 "
                                "0B 29 02 00 01 0B 2A 02 00 01 00 02 0B 20 08 00 00 0B 21 08 00 00 0B 2B 02 00 01 0B "
                                "2C 02 00 01 00 03 0B 20 08 00 00 0B 21 08 00 00 0B 2D 08 00 00 0B 22 0C 00 19 0B 23 "
                                "0C 00 19 0B 2E 0C 00 04 0B 2F 0A 00 01 0B 30 02 00 00 00 04 0B 20 08 00 00 0B 21 08 "
                                "00 00 0B 22 0C 00 19 0B 23 0C 00 19 0B 1B 02 00 01 0B 31 02 00 01 0B 32 02 00 01 0B "
                                "33 02 00 01 0B 24 0C 00 0E 0B 34 0C 00 11 0B 35 02 00 01 0B 36 02 00 01 0B 37 02 00 "
                                "01 0B 38 02 00 01 0B 39 08 02 00 0B 3A 08 02 00 0B 3B 02 00 00 00 05 0B 2F 0C 00 04 "
                                "0B 3C 0C 00 04 0B 3D 0C 00 10 00 06 0B 3E 00 00 00 0B 3F 0A 00 00 00 07 0B C0 00 02 "
                                "00 00 0B C1 00 02 00 01 0B C2 00 02 00 01 0B C3 00 02 00 01 0B C4 00 0D 00 06 00 08 "
                                "0B 20 08 00 00 0B 21 08 00 00 0B 22 0C 00 19 0B 23 0C 00 19 0B C5 00 0C 00 17 0B 33 "
                                "0C 00 15 0B C6 00 02 00 00 0B C7 00 02 00 00 00 09 0B C8 00 02 00 01 00 0A 0B 20 08 "
                                "00 00 0B 21 08 00 00 0B 22 0C 00 19 0B 23 0C 00 19 0B C9 00 0C 00 10 0B 24 0C 00 0E "
                                "0B CA 00 0C 00 19 0B CB 00 02 00 00 0B CC 00 01 00 00 0B 3B 02 00 00 00 0B 0B 20 08 "
                                "00 00 0B 21 08 00 00 0B 22 0C 00 19 0B 23 0C 00 19 0B CD 00 0C 00 04 0B CE 00 08 00 "
                                "00 0B CB 00 02 00 00 0B CC 00 01 00 00 00 0C 0B CF 00 02 00 00 0B D0 00 01 00 00 00 "
                                "0D 0B 20 08 00 00 0B 21 08 00 00 0B 22 0C 00 19 0B 23 0C 00 19 0B 24 0C 00 0E 0B D1 "
                                "00 08 00 00 0B D2 00 08 00 01 0B D3 00 02 00 01 0B D4 00 02 00 01 0B 32 02 00 01 0B "
                                "D5 00 02 00 01 0B D6 00 0D 00 06 0B D7 00 02 00 01 0B D8 00 02 00 01 0B 36 02 00 01 "
                                "0B 37 02 00 01 0B 35 02 00 01 0B 38 02 00 01 0B C7 00 02 00 00 0B D9 00 02 00 00 0B "
                                "DA 00 00 00 01 0B DB 00 02 00 01 0B DC 00 02 00 01 0B DD 00 0D 00 15 0B DE 00 0D 00 "
                                "17 0B DF 00 0D 00 12 0B E0 00 00 00 01 0B E1 00 0A 00 01 0B E2 00 02 00 00 0B E3 00 "
                                "01 00 00 0B E4 00 01 00 00 0B E5 00 00 00 00 0B E6 00 02 00 00 0B E7 00 02 00 00 00 "
                                "0E 0B 20 08 00 00 0B 21 08 00 00 0B 22 0C 00 19 0B 23 0C 00 19 0B E8 00 0C 00 14 0B "
                                "E9 00 0D 00 15 0B EA 00 0D 00 15 0B EB 00 0C 00 0F 0B EC 00 01 00 00 0B ED 00 08 00 "
                                "01 0B EE 00 01 00 00 0B 34 0C 00 11 0B EF 00 08 02 00 0B F0 00 08 02 00 0B F1 00 02 "
                                "00 00 00 0F 0B 20 08 00 00 0B 21 08 00 00 0B 22 0C 00 19 0B 23 0C 00 19 00 10 0B 20 "
                                "08 00 00 0B 21 08 00 00 0B F2 00 08 00 01 0B 22 0C 00 19 0B 23 0C 00 19 0B F3 00 0D "
                                "00 08 0B CC 00 01 00 00 0B F4 00 00 00 01 0B F5 00 00 00 01 0B F6 00 00 00 01 0B F7 "
                                "00 02 00 00 0B F8 00 02 00 00 0B F9 00 02 00 00 0B 2E 0C 00 0D 0B 2A 02 00 01 0B EE "
                                "00 01 00 00 00 11 0B 20 08 00 00 0B 21 08 00 00 0B FA 00 0C 00 02 0B 36 02 00 01 0B "
                                "FB 00 02 00 01 0B EA 00 0D 00 16 0B FC 00 02 00 01 0B FD 00 02 00 01 0B 1B 02 00 01 "
                                "0B FE 00 02 00 01 0B FF 00 02 00 01 0B 80 01 02 00 01 0B 81 01 02 00 01 0B 82 01 02 "
                                "00 01 0B 32 02 00 01 0B 83 01 02 00 01 0B 84 01 02 00 01 0B 85 01 02 00 01 0B 86 01 "
                                "02 00 01 0B 87 01 02 00 01 0B 88 01 02 00 01 0B 89 01 02 00 01 0B D8 00 02 00 01 0B "
                                "8A 01 02 00 01 0B 8B 01 02 00 01 0B 8C 01 02 00 01 0B 8D 01 02 00 01 0B 8E 01 02 00 "
                                "01 0B D5 00 0D 00 06 00 12 0B 8F 01 02 00 00 00 13 0B 20 08 00 00 0B 21 08 00 00 0B "
                                "22 0C 00 19 0B 23 0C 00 19 0B 90 01 0C 00 14 0B 91 01 02 00 01 0B 92 01 02 00 01 0B "
                                "93 01 02 00 01 0B 94 01 02 00 01 0B 95 01 02 00 01 0B 96 01 02 00 01 0B 97 01 02 00 "
                                "01 0B D1 00 08 00 01 0B D2 00 08 00 01 0B 98 01 0A 00 01 0B 99 01 0A 00 01 00 14 0B "
                                "20 08 00 00 0B 21 08 00 00 0B 1B 02 00 01 0B 9A 01 02 00 01 0B 9B 01 02 00 01 0B 9C "
                                "01 08 00 01 0B 9D 01 0C 00 19 0B 9E 01 0C 00 19 0B 9F 01 02 00 01 0B A0 01 01 00 00 "
                                "00 15 0B 20 08 00 00 0B 21 08 00 00 0B 22 0C 00 19 0B 23 0C 00 19 0B A1 01 02 00 01 "
                                "0B A2 01 08 00 01 00 16 0B 20 08 00 00 0B 21 08 00 00 0B A1 01 02 00 01 00 17 0B 20 "
                                "08 00 00 0B 21 08 00 00 0B 22 0C 00 19 0B 23 0C 00 19 0B A3 01 02 00 01 0B F8 00 02 "
                                "00 00 00 18 0B A4 01 08 00 00 0B CB 00 02 00 01 00 19 0B 20 08 00 00 0B 21 08 00 00 "
                                "0B A5 01 02 02 00 0B A6 01 0D 00 14 0B A7 01 0D 00 0C 0B 1B 02 00 01 0B A8 01 02 00 "
                                "01 0B A9 01 02 00 01 0B 9F 01 02 00 01 0B AA 01 02 00 00 0B AB 01 02 02 00 00 1A 0B "
                                "AC 01 0D 00 1C 00 1C 0B AD 01 0C 00 1E 0B AE 01 01 00 00 0B AF 01 01 00 00 0B B0 01 "
                                "01 00 00 0B B1 01 01 00 00 0B B2 01 01 00 00 0B B3 01 01 00 00 0B B4 01 01 00 00 00 "
                                "1D 0B AC 01 0D 00 1C 00 1E 0B B5 01 0D 00 1F 00 1F 0B AD 01 0C 00 1E";

    std::vector<char> changeset_vec;
    {
        std::istringstream in{changeset_hex};
        int n;
        in >> std::hex >> n;
        while (in) {
            BARQ_ASSERT(n >= 0 && n <= 255);
            changeset_vec.push_back(n);
            in >> std::hex >> n;
        }
    }

    BinaryData changeset_bin{changeset_vec.data(), changeset_vec.size()};

    file_ident_type client_file_ident = 51;
    timestamp_type origin_timestamp = 103573722140;
    file_ident_type origin_file_ident = 0;
    version_type client_version = 2;
    version_type last_integrated_server_version = 0;
    UploadCursor upload_cursor{client_version, last_integrated_server_version};

    _impl::ServerHistory::IntegratableChangeset integratable_changeset{
        client_file_ident, origin_timestamp, origin_file_ident, upload_cursor, changeset_bin};

    _impl::ServerHistory::IntegratableChangesets integratable_changesets;
    integratable_changesets[client_file_ident].changesets.push_back(integratable_changeset);

    issue2104::ServerHistoryContext history_context;
    _impl::ServerHistory history{history_context};
    DBRef db = DB::create(history, barq_path_copy);

    VersionInfo version_info;
    bool backup_whole_barq;
    _impl::ServerHistory::IntegrationResult result;
    history.integrate_client_changesets(integratable_changesets, version_info, backup_whole_barq, result,
                                        *test_context.logger);
}


TEST(Sync_RunServerWithoutPublicKey)
{
    TEST_CLIENT_DB(db);
    TEST_DIR(server_dir);
    ClientServerFixture::Config config;
    config.server_public_key_path = {};
    ClientServerFixture fixture(server_dir, test_context, std::move(config));
    fixture.start();

    // Server must accept an unsigned token when a public key is not passed to
    // it
    {
        Session session = fixture.make_bound_session(db, "/test", g_unsigned_test_user_token);
        session.wait_for_download_complete_or_client_stopped();
    }

    // Server must also accept a signed token when a public key is not passed to
    // it
    {
        Session session = fixture.make_bound_session(db, "/test");
        session.wait_for_download_complete_or_client_stopped();
    }
}


TEST(Sync_ServerSideEncryption)
{
    TEST_CLIENT_DB(db);
    {
        WriteTransaction wt(db);
        wt.get_group().add_table_with_primary_key("class_Test", type_Int, "id");
        wt.commit();
    }

    TEST_DIR(server_dir);
    bool always_encrypt = true;
    std::string server_path;
    {
        ClientServerFixture::Config config;
        if (auto key = crypt_key(always_encrypt)) {
            config.server_encryption_key.emplace();
            memcpy(config.server_encryption_key->data(), key, config.server_encryption_key->size());
        }
        ClientServerFixture fixture(server_dir, test_context, std::move(config));
        fixture.start();

        Session session = fixture.make_bound_session(db, "/test");
        session.wait_for_upload_complete_or_client_stopped();

        server_path = fixture.map_virtual_to_real_path("/test");
    }

    const char* encryption_key = crypt_key(always_encrypt);
    Group group{server_path, encryption_key};
    CHECK(group.has_table("class_Test"));
}

TEST(Sync_LogCompaction_EraseObject_LinkList)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);
    ClientServerFixture::Config config;

    // Log comapction is true by default, but we emphasize it.
    config.disable_upload_compaction = false;

    ClientServerFixture fixture(dir, test_context, std::move(config));
    fixture.start();

    {
        WriteTransaction wt{db_1};

        TableRef table_source = wt.get_group().add_table_with_primary_key("class_source", type_Int, "id");
        TableRef table_target = wt.get_group().add_table_with_primary_key("class_target", type_Int, "id");
        auto col_key = table_source->add_column_list(*table_target, "target_link");

        auto k0 = table_target->create_object_with_primary_key(1).get_key();
        auto k1 = table_target->create_object_with_primary_key(2).get_key();

        auto ll = table_source->create_object_with_primary_key(0).get_linklist_ptr(col_key);
        ll->add(k0);
        ll->add(k1);
        CHECK_EQUAL(ll->size(), 2);
        wt.commit();
    }

    {
        Session session_1 = fixture.make_bound_session(db_1);
        Session session_2 = fixture.make_bound_session(db_2);

        session_1.wait_for_upload_complete_or_client_stopped();
        session_2.wait_for_download_complete_or_client_stopped();
    }

    {
        WriteTransaction wt{db_1};

        TableRef table_source = wt.get_table("class_source");
        TableRef table_target = wt.get_table("class_target");

        CHECK_EQUAL(table_source->size(), 1);
        CHECK_EQUAL(table_target->size(), 2);

        table_target->get_object(1).remove();
        table_target->get_object(0).remove();

        table_source->get_object(0).remove();
        wt.commit();
    }

    {
        WriteTransaction wt{db_2};

        TableRef table_source = wt.get_table("class_source");
        TableRef table_target = wt.get_table("class_target");
        auto col_key = table_source->get_column_key("target_link");

        CHECK_EQUAL(table_source->size(), 1);
        CHECK_EQUAL(table_target->size(), 2);

        auto k0 = table_target->begin()->get_key();

        auto ll = table_source->get_object(0).get_linklist_ptr(col_key);
        ll->add(k0);
        wt.commit();
    }

    {
        Session session_1 = fixture.make_bound_session(db_1);
        session_1.wait_for_upload_complete_or_client_stopped();
    }

    {
        Session session_2 = fixture.make_bound_session(db_2);
        session_2.wait_for_upload_complete_or_client_stopped();
        session_2.wait_for_download_complete_or_client_stopped();
    }

    {
        ReadTransaction rt{db_2};

        ConstTableRef table_source = rt.get_group().get_table("class_source");
        ConstTableRef table_target = rt.get_group().get_table("class_target");

        CHECK_EQUAL(table_source->size(), 0);
        CHECK_EQUAL(table_target->size(), 0);
    }
}


// This test could trigger the assertion that the row_for_object_id cache is
// valid before the cache was properly invalidated in the case of a short
// circuited sync replicator.
TEST(Sync_CreateObjects_EraseObjects)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);
    ClientServerFixture fixture(dir, test_context);
    fixture.start();

    Session session_1 = fixture.make_bound_session(db_1);
    Session session_2 = fixture.make_bound_session(db_2);

    write_transaction(db_1, [](WriteTransaction& wt) {
        TableRef table = wt.get_group().add_table_with_primary_key("class_persons", type_Int, "id");
        table->create_object_with_primary_key(1);
        table->create_object_with_primary_key(2);
    });
    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();

    write_transaction(db_1, [&](WriteTransaction& wt) {
        TableRef table = wt.get_table("class_persons");
        CHECK_EQUAL(table->size(), 2);
        table->get_object(0).remove();
        table->get_object(0).remove();
    });
    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();
}


TEST(Sync_CreateDeleteCreateTableWithPrimaryKey)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db);
    ClientServerFixture fixture(dir, test_context);
    fixture.start();

    Session session = fixture.make_bound_session(db);

    write_transaction(db, [](WriteTransaction& wt) {
        TableRef table = wt.get_group().add_table_with_primary_key("class_t", type_Int, "pk");
        wt.get_group().remove_table(table->get_key());
        table = wt.get_group().add_table_with_primary_key("class_t", type_String, "pk");
    });
    session.wait_for_upload_complete_or_client_stopped();
    session.wait_for_download_complete_or_client_stopped();
}

template <typename T>
T sequence_next()
{
    BARQ_UNREACHABLE();
}

template <>
ObjectId sequence_next()
{
    return ObjectId::gen();
}

template <>
UUID sequence_next()
{
    union {
        struct {
            uint64_t upper;
            uint64_t lower;
        } ints;
        UUID::UUIDBytes bytes;
    } u;
    static uint64_t counter = test_util::random_int(0, 1000);
    u.ints.upper = ++counter;
    u.ints.lower = ++counter;
    return UUID{u.bytes};
}

template <>
Int sequence_next()
{
    static Int count = test_util::random_int(-1000, 1000);
    return ++count;
}

template <>
String sequence_next()
{
    static std::string str;
    static Int sequence = test_util::random_int(-1000, 1000);
    str = util::format("string sequence %1", ++sequence);
    return String(str);
}

NONCONCURRENT_TEST_TYPES(Sync_PrimaryKeyTypes, Int, String, ObjectId, UUID, util::Optional<Int>,
                         util::Optional<ObjectId>, util::Optional<UUID>)
{
    using underlying_type = typename util::RemoveOptional<TEST_TYPE>::type;
    constexpr bool is_optional = !std::is_same_v<underlying_type, TEST_TYPE>;
    DataType type = ColumnTypeTraits<TEST_TYPE>::id;

    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    TEST_DIR(dir);
    fixtures::ClientServerFixture fixture{dir, test_context};
    fixture.start();

    Session session_1 = fixture.make_session(db_1, "/test");
    Session session_2 = fixture.make_session(db_2, "/test");

    TEST_TYPE obj_1_id;
    TEST_TYPE obj_2_id;

    TEST_TYPE default_or_null{};
    if constexpr (std::is_same_v<TEST_TYPE, String>) {
        default_or_null = "";
    }
    if constexpr (is_optional) {
        CHECK(!default_or_null);
    }

    {
        WriteTransaction tr{db_1};
        auto table_1 = tr.get_group().add_table_with_primary_key("class_Table1", type, "id", is_optional);
        auto table_2 = tr.get_group().add_table_with_primary_key("class_Table2", type, "id", is_optional);
        table_1->add_column_list(type, "oids", is_optional);

        auto obj_1 = table_1->create_object_with_primary_key(sequence_next<underlying_type>());
        auto obj_2 = table_2->create_object_with_primary_key(sequence_next<underlying_type>());
        if constexpr (is_optional) {
            table_2->create_object_with_primary_key(default_or_null);
        }

        auto list = obj_1.template get_list<TEST_TYPE>("oids");
        obj_1_id = obj_1.template get<TEST_TYPE>("id");
        obj_2_id = obj_2.template get<TEST_TYPE>("id");
        list.insert(0, obj_2_id);
        list.insert(1, default_or_null);
        list.add(default_or_null);

        tr.commit();
    }

    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();

    {
        ReadTransaction tr{db_2};
        auto table_1 = tr.get_table("class_Table1");
        auto table_2 = tr.get_table("class_Table2");
        auto obj_1 = *table_1->begin();
        auto obj_2 = table_2->find_first(table_2->get_column_key("id"), obj_2_id);
        CHECK(obj_2);
        auto list = obj_1.get_list<TEST_TYPE>("oids");
        CHECK_EQUAL(obj_1.template get<TEST_TYPE>("id"), obj_1_id);
        CHECK_EQUAL(list.size(), 3);
        CHECK_NOT(list.is_null(0));
        CHECK_EQUAL(list.get(0), obj_2_id);
        CHECK_EQUAL(list.get(1), default_or_null);
        CHECK_EQUAL(list.get(2), default_or_null);
        if constexpr (is_optional) {
            auto obj_3 = table_2->find_first_null(table_2->get_column_key("id"));
            CHECK(obj_3);
            CHECK(list.is_null(1));
            CHECK(list.is_null(2));
        }
    }
}

TEST(Sync_Mixed)
{
    // Test replication and synchronization of Mixed values and lists.
    DBOptions options;
    options.logger = test_context.logger;
    SHARED_GROUP_TEST_PATH(db_1_path);
    SHARED_GROUP_TEST_PATH(db_2_path);
    auto db_1 = DB::create(make_client_replication(), db_1_path, options);
    auto db_2 = DB::create(make_client_replication(), db_2_path, options);

    TEST_DIR(dir);
    fixtures::ClientServerFixture fixture{dir, test_context};
    fixture.start();

    Session session_1 = fixture.make_session(db_1, "/test");
    Session session_2 = fixture.make_session(db_2, "/test");

    {
        WriteTransaction tr{db_1};
        auto& g = tr.get_group();
        auto foos = g.add_table_with_primary_key("class_Foo", type_Int, "id");
        auto bars = g.add_table_with_primary_key("class_Bar", type_String, "id");
        auto fops = g.add_table_with_primary_key("class_Fop", type_Int, "id");
        foos->add_column(type_Mixed, "value", true);
        foos->add_column_list(type_Mixed, "values");

        auto foo = foos->create_object_with_primary_key(123);
        auto bar = bars->create_object_with_primary_key("Hello");
        auto fop = fops->create_object_with_primary_key(456);

        foo.set("value", Mixed(6.2f));
        auto values = foo.get_list<Mixed>("values");
        values.insert(0, StringData("A"));
        values.insert(1, ObjLink{bars->get_key(), bar.get_key()});
        values.insert(2, ObjLink{fops->get_key(), fop.get_key()});
        values.insert(3, 123.f);

        tr.commit();
    }

    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();

    {
        ReadTransaction tr{db_2};

        auto foos = tr.get_table("class_Foo");
        auto bars = tr.get_table("class_Bar");
        auto fops = tr.get_table("class_Fop");

        CHECK_EQUAL(foos->size(), 1);
        CHECK_EQUAL(bars->size(), 1);
        CHECK_EQUAL(fops->size(), 1);

        auto foo = *foos->begin();
        auto value = foo.get<Mixed>("value");
        CHECK_EQUAL(value, Mixed{6.2f});
        auto values = foo.get_list<Mixed>("values");
        CHECK_EQUAL(values.size(), 4);

        auto v0 = values.get(0);
        auto v1 = values.get(1);
        auto v2 = values.get(2);
        auto v3 = values.get(3);

        auto l1 = v1.get_link();
        auto l2 = v2.get_link();

        auto l1_table = tr.get_table(l1.get_table_key());
        auto l2_table = tr.get_table(l2.get_table_key());

        CHECK_EQUAL(v0, Mixed{"A"});
        CHECK_EQUAL(l1_table, bars);
        CHECK_EQUAL(l2_table, fops);
        CHECK_EQUAL(l1.get_obj_key(), bars->begin()->get_key());
        CHECK_EQUAL(l2.get_obj_key(), fops->begin()->get_key());
        CHECK_EQUAL(v3, Mixed{123.f});
    }
}

/*
TEST(Sync_TypedLinks)
{
    // Test replication and synchronization of Mixed values and lists.

    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    TEST_DIR(dir);
    fixtures::ClientServerFixture fixture{dir, test_context};
    fixture.start();

    Session session_1 = fixture.make_session(db_1, "/test");
    Session session_2 = fixture.make_session(db_2, "/test");

    write_transaction(db_1, [](WriteTransaction& tr) {
        auto& g = tr.get_group();
        auto foos = g.add_table_with_primary_key("class_Foo", type_Int, "id");
        auto bars = g.add_table_with_primary_key("class_Bar", type_String, "id");
        auto fops = g.add_table_with_primary_key("class_Fop", type_Int, "id");
        foos->add_column(type_TypedLink, "link");

        auto foo1 = foos->create_object_with_primary_key(123);
        auto foo2 = foos->create_object_with_primary_key(456);
        auto bar = bars->create_object_with_primary_key("Hello");
        auto fop = fops->create_object_with_primary_key(456);

        foo1.set("link", ObjLink(bars->get_key(), bar.get_key()));
        foo2.set("link", ObjLink(fops->get_key(), fop.get_key()));
    });

    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();

    {
        ReadTransaction tr{db_2};

        auto foos = tr.get_table("class_Foo");
        auto bars = tr.get_table("class_Bar");
        auto fops = tr.get_table("class_Fop");

        CHECK_EQUAL(foos->size(), 2);
        CHECK_EQUAL(bars->size(), 1);
        CHECK_EQUAL(fops->size(), 1);

        auto it = foos->begin();
        auto l1 = it->get<ObjLink>("link");
        ++it;
        auto l2 = it->get<ObjLink>("link");

        auto l1_table = tr.get_table(l1.get_table_key());
        auto l2_table = tr.get_table(l2.get_table_key());

        CHECK_EQUAL(l1_table, bars);
        CHECK_EQUAL(l2_table, fops);
        CHECK_EQUAL(l1.get_obj_key(), bars->begin()->get_key());
        CHECK_EQUAL(l2.get_obj_key(), fops->begin()->get_key());
    }
}
*/

TEST(Sync_Dictionary)
{
    // Test replication and synchronization of Mixed values and lists.

    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    TEST_DIR(dir);
    fixtures::ClientServerFixture fixture{dir, test_context};
    fixture.start();

    Session session_1 = fixture.make_session(db_1, "/test");
    Session session_2 = fixture.make_session(db_2, "/test");

    Timestamp now{std::chrono::system_clock::now()};

    write_transaction(db_1, [&](WriteTransaction& tr) {
        auto& g = tr.get_group();
        auto foos = g.add_table_with_primary_key("class_Foo", type_Int, "id");
        auto col_dict = foos->add_column_dictionary(type_Mixed, "dict");
        auto col_dict_str = foos->add_column_dictionary(type_String, "str_dict", true);

        auto foo = foos->create_object_with_primary_key(123);

        auto dict = foo.get_dictionary(col_dict);
        dict.insert("hello", "world");
        dict.insert("cnt", 7);
        dict.insert("when", now);

        auto dict_str = foo.get_dictionary(col_dict_str);
        dict_str.insert("some", "text");
        dict_str.insert("nothing", null());
    });

    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();

    write_transaction(db_2, [&](WriteTransaction& tr) {
        auto foos = tr.get_table("class_Foo");
        CHECK_EQUAL(foos->size(), 1);

        auto it = foos->begin();
        auto dict = it->get_dictionary(foos->get_column_key("dict"));
        CHECK(dict.get_value_data_type() == type_Mixed);
        CHECK_EQUAL(dict.size(), 3);

        auto col_dict_str = foos->get_column_key("str_dict");
        auto dict_str = it->get_dictionary(col_dict_str);
        CHECK(col_dict_str.is_nullable());
        CHECK(dict_str.get_value_data_type() == type_String);
        CHECK_EQUAL(dict_str.size(), 2);

        Mixed val = dict["hello"];
        CHECK_EQUAL(val.get_string(), "world");
        val = dict.get("cnt");
        CHECK_EQUAL(val.get_int(), 7);
        val = dict.get("when");
        CHECK_EQUAL(val.get<Timestamp>(), now);

        dict.erase("cnt");
        dict.insert("hello", "goodbye");
    });

    session_2.wait_for_upload_complete_or_client_stopped();
    session_1.wait_for_download_complete_or_client_stopped();

    write_transaction(db_1, [&](WriteTransaction& tr) {
        auto foos = tr.get_table("class_Foo");
        CHECK_EQUAL(foos->size(), 1);

        auto it = foos->begin();
        auto dict = it->get_dictionary(foos->get_column_key("dict"));
        CHECK_EQUAL(dict.size(), 2);

        Mixed val = dict["hello"];
        CHECK_EQUAL(val.get_string(), "goodbye");
        val = dict.get("when");
        CHECK_EQUAL(val.get<Timestamp>(), now);

        dict.clear();
    });

    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();

    {
        ReadTransaction read_1{db_1};
        ReadTransaction read_2{db_2};
        // tr.get_group().to_json(std::cout);

        auto foos = read_2.get_table("class_Foo");

        CHECK_EQUAL(foos->size(), 1);

        auto it = foos->begin();
        auto dict = it->get_dictionary(foos->get_column_key("dict"));
        CHECK_EQUAL(dict.size(), 0);

        CHECK(compare_groups(read_1, read_2));
    }
}

TEST(Sync_CollectionInMixed)
{
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    TEST_DIR(dir);
    fixtures::ClientServerFixture fixture{dir, test_context};
    fixture.start();

    Session session_1 = fixture.make_session(db_1, "/test");
    Session session_2 = fixture.make_session(db_2, "/test");

    Timestamp now{std::chrono::system_clock::now()};

    write_transaction(db_1, [&](WriteTransaction& tr) {
        auto& g = tr.get_group();
        auto table = g.add_table_with_primary_key("class_Table", type_Int, "id");
        auto col_any = table->add_column(type_Mixed, "any");

        auto foo = table->create_object_with_primary_key(123);

        // Create dictionary in Mixed property
        foo.set_collection(col_any, CollectionType::Dictionary);
        auto dict = foo.get_dictionary_ptr(col_any);
        dict->insert("hello", "world");
        dict->insert("cnt", 7);
        dict->insert("when", now);
        // Insert a List in a Dictionary
        dict->insert_collection("list", CollectionType::List);
        auto l = dict->get_list("list");
        l->add(5);
        l->insert_collection(1, CollectionType::List);
        l->get_list(1)->add(7);

        auto bar = table->create_object_with_primary_key(456);

        // Create list in Mixed property
        bar.set_collection(col_any, CollectionType::List);
        auto list = bar.get_list_ptr<Mixed>(col_any);
        list->add("John");
        list->insert(0, 5);
    });

    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();

    write_transaction(db_2, [&](WriteTransaction& tr) {
        auto table = tr.get_table("class_Table");
        auto col_any = table->get_column_key("any");
        CHECK_EQUAL(table->size(), 2);

        auto obj = table->get_object_with_primary_key(123);
        auto dict = obj.get_dictionary_ptr(col_any);
        CHECK(dict->get_value_data_type() == type_Mixed);
        CHECK_EQUAL(dict->size(), 4);

        // Check that values are replicated
        Mixed val = dict->get("hello");
        CHECK_EQUAL(val.get_string(), "world");
        val = dict->get("cnt");
        CHECK_EQUAL(val.get_int(), 7);
        val = dict->get("when");
        CHECK_EQUAL(val.get<Timestamp>(), now);
        CHECK_EQUAL(dict->get_list("list")->get(0).get_int(), 5);

        // Erase dictionary element
        dict->erase("cnt");
        // Replace dictionary element
        dict->insert("hello", "goodbye");

        obj = table->get_object_with_primary_key(456);
        auto list = obj.get_list_ptr<Mixed>(col_any);
        // Check that values are replicated
        CHECK_EQUAL(list->get(0).get_int(), 5);
        CHECK_EQUAL(list->get(1).get_string(), "John");
        // Replace list element
        list->set(1, "Paul");
        // Erase list element
        list->remove(0);
    });

    session_2.wait_for_upload_complete_or_client_stopped();
    session_1.wait_for_download_complete_or_client_stopped();

    write_transaction(db_1, [&](WriteTransaction& tr) {
        auto table = tr.get_table("class_Table");
        auto col_any = table->get_column_key("any");
        CHECK_EQUAL(table->size(), 2);

        auto obj = table->get_object_with_primary_key(123);
        auto dict = obj.get_dictionary(col_any);
        CHECK_EQUAL(dict.size(), 3);

        Mixed val = dict["hello"];
        CHECK_EQUAL(val.get_string(), "goodbye");
        val = dict.get("when");
        CHECK_EQUAL(val.get<Timestamp>(), now);

        // Dictionary clear
        dict.clear();

        obj = table->get_object_with_primary_key(456);
        auto list = obj.get_list_ptr<Mixed>(col_any);
        CHECK_EQUAL(list->size(), 1);
        CHECK_EQUAL(list->get(0).get_string(), "Paul");
        // List clear
        list->clear();
    });

    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();

    write_transaction(db_2, [&](WriteTransaction& tr) {
        auto table = tr.get_table("class_Table");
        auto col_any = table->get_column_key("any");

        CHECK_EQUAL(table->size(), 2);

        auto obj = table->get_object_with_primary_key(123);
        auto dict = obj.get_dictionary(col_any);
        CHECK_EQUAL(dict.size(), 0);

        // Replace dictionary with list on property
        obj.set_collection(col_any, CollectionType::List);

        obj = table->get_object_with_primary_key(456);
        auto list = obj.get_list<Mixed>(col_any);
        CHECK_EQUAL(list.size(), 0);
        // Replace list with Dictionary on property
        obj.set_collection(col_any, CollectionType::Dictionary);
    });

    session_2.wait_for_upload_complete_or_client_stopped();
    session_1.wait_for_download_complete_or_client_stopped();

    {
        ReadTransaction read_1{db_1};
        ReadTransaction read_2{db_2};

        auto table = read_2.get_table("class_Table");
        auto col_any = table->get_column_key("any");

        CHECK_EQUAL(table->size(), 2);

        auto obj = table->get_object_with_primary_key(123);
        auto list = obj.get_list<Mixed>(col_any);
        CHECK_EQUAL(list.size(), 0);

        obj = table->get_object_with_primary_key(456);
        auto dict = obj.get_dictionary(col_any);
        CHECK_EQUAL(dict.size(), 0);

        CHECK(compare_groups(read_1, read_2));
    }
}

TEST(Sync_CollectionInCollection)
{
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    TEST_DIR(dir);
    fixtures::ClientServerFixture fixture{dir, test_context};
    fixture.start();

    Session session_1 = fixture.make_session(db_1, "/test");
    Session session_2 = fixture.make_session(db_2, "/test");

    Timestamp now{std::chrono::system_clock::now()};

    write_transaction(db_1, [&](WriteTransaction& tr) {
        auto& g = tr.get_group();
        auto table = g.add_table_with_primary_key("class_Table", type_Int, "id");
        auto col_any = table->add_column(type_Mixed, "any");

        auto foo = table->create_object_with_primary_key(123);

        // Create dictionary in Mixed property
        foo.set_collection(col_any, CollectionType::Dictionary);
        auto dict = foo.get_dictionary_ptr(col_any);
        dict->insert("hello", "world");
        dict->insert("cnt", 7);
        dict->insert("when", now);
        // Insert a List in a Dictionary
        dict->insert_collection("collection", CollectionType::List);
        auto l = dict->get_list("collection");
        l->add(5);

        auto bar = table->create_object_with_primary_key(456);

        // Create list in Mixed property
        bar.set_collection(col_any, CollectionType::List);
        auto list = bar.get_list_ptr<Mixed>(col_any);
        list->add("John");
        list->insert(0, 5);
        // Insert dictionary in List
        list->insert_collection(2, CollectionType::Dictionary);
        auto d = list->get_dictionary(2);
        d->insert("One", 1);
        d->insert("Two", 2);
    });

    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();

    write_transaction(db_2, [&](WriteTransaction& tr) {
        auto table = tr.get_table("class_Table");
        auto col_any = table->get_column_key("any");
        CHECK_EQUAL(table->size(), 2);

        auto obj = table->get_object_with_primary_key(123);
        auto dict = obj.get_dictionary_ptr(col_any);
        CHECK(dict->get_value_data_type() == type_Mixed);
        CHECK_EQUAL(dict->size(), 4);

        // Replace List with Dictionary
        dict->insert_collection("collection", CollectionType::Dictionary);
        auto d = dict->get_dictionary("collection");
        d->insert("Three", 3);
        d->insert("Four", 4);

        obj = table->get_object_with_primary_key(456);
        auto list = obj.get_list_ptr<Mixed>(col_any);
        // Replace Dictionary with List
        list->set_collection(2, CollectionType::List);
        auto l = list->get_list(2);
        l->add(47);
    });

    session_2.wait_for_upload_complete_or_client_stopped();
    session_1.wait_for_download_complete_or_client_stopped();

    {
        ReadTransaction read_1{db_1};
        ReadTransaction read_2{db_2};

        auto table = read_2.get_table("class_Table");
        auto col_any = table->get_column_key("any");

        CHECK_EQUAL(table->size(), 2);

        auto obj = table->get_object_with_primary_key(123);
        auto dict = obj.get_dictionary_ptr(col_any);
        auto d = dict->get_dictionary("collection");
        CHECK_EQUAL(d->get("Four").get_int(), 4);

        obj = table->get_object_with_primary_key(456);
        auto list = obj.get_list_ptr<Mixed>(col_any);
        auto l = list->get_list(2);
        CHECK_EQUAL(l->get_any(0).get_int(), 47);
    }
}

TEST(Sync_DeleteCollectionInCollection)
{
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    TEST_DIR(dir);
    fixtures::ClientServerFixture fixture{dir, test_context};
    fixture.start();

    Session session_1 = fixture.make_session(db_1, "/test");
    Session session_2 = fixture.make_session(db_2, "/test");

    Timestamp now{std::chrono::system_clock::now()};

    write_transaction(db_1, [&](WriteTransaction& tr) {
        auto& g = tr.get_group();
        auto table = g.add_table_with_primary_key("class_Table", type_Int, "id");
        auto col_any = table->add_column(type_Mixed, "any");

        auto foo = table->create_object_with_primary_key(123);

        // Create list in Mixed property
        foo.set_json(col_any, R"([
            {
              "1_map": {
                "2_string": "map value"
               },
              "1_list": ["list value"]
            }
        ])");
    });

    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();

    write_transaction(db_2, [&](WriteTransaction& tr) {
        auto table = tr.get_table("class_Table");
        auto col_any = table->get_column_key("any");
        CHECK_EQUAL(table->size(), 1);

        auto obj = table->get_object_with_primary_key(123);
        auto list = obj.get_list<Mixed>(col_any);
        auto dict = list.get_dictionary(0);
        dict->erase("1_map");
    });

    session_2.wait_for_upload_complete_or_client_stopped();
    session_1.wait_for_download_complete_or_client_stopped();

    {
        ReadTransaction read_1{db_1};

        auto table = read_1.get_table("class_Table");
        auto col_any = table->get_column_key("any");

        CHECK_EQUAL(table->size(), 1);

        auto obj = table->get_object_with_primary_key(123);
        auto list = obj.get_list<Mixed>(col_any);
        auto dict = list.get_dictionary(0);
        CHECK_EQUAL(dict->size(), 1);
    }
}

TEST(Sync_NestedCollectionClear)
{
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    TEST_DIR(dir);
    fixtures::ClientServerFixture fixture{dir, test_context};
    fixture.start();

    Session session_1 = fixture.make_session(db_1, "/test");
    Session session_2 = fixture.make_session(db_2, "/test");

    auto tr_1 = db_1->start_write();
    auto tr_2 = db_2->start_read();
    auto table_1 = tr_1->add_table_with_primary_key("class_Table", type_Int, "id");
    auto col = table_1->add_column(type_Mixed, "any");
    table_1->add_column_list(type_Mixed, "ints");
    auto col_list = table_1->add_column_list(type_Mixed, "any_list");

    auto foo = table_1->create_object_with_primary_key(123);
    foo.set_collection(col, CollectionType::List);
    auto parent_list = foo.get_list<Mixed>(col_list);
    parent_list.insert_collection(0, CollectionType::List);
    parent_list.insert_collection(1, CollectionType::Dictionary);
    auto foo1 = table_1->create_object_with_primary_key(456);
    foo1.set_collection(col, CollectionType::Dictionary);
    tr_1->commit_and_continue_as_read();

    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();

    {
        tr_1->promote_to_write();
        auto list = foo.get_list<Mixed>("any");
        list.clear();
        list.add("Hello");

        list = foo.get_list<Mixed>("any_list");
        auto sub_list = list.get_list(0);
        sub_list->clear();
        sub_list->add(1);
        sub_list->add(2);
        auto sub_dict = list.get_dictionary(1);
        sub_dict->clear();
        sub_dict->insert("one", 1);
        sub_dict->insert("two", 2);

        auto dict = foo1.get_dictionary("any");
        dict.clear();
        dict.insert("age", 42);

        auto list_int = foo.get_list<Mixed>("ints");
        list_int.clear();
        list_int.add(1);
        list_int.add(2);
        tr_1->commit_and_continue_as_read();
    }

    {
        tr_2->promote_to_write();
        auto table_2 = tr_2->get_table("class_Table");

        auto bar = table_2->get_object_with_primary_key(123);
        auto list = bar.get_list<Mixed>("any");
        list.clear();
        list.add("Godbye");

        list = bar.get_list<Mixed>("any_list");
        auto sub_list = list.get_list(0);
        sub_list->clear();
        sub_list->add(3);
        sub_list->add(4);
        auto sub_dict = list.get_dictionary(1);
        sub_dict->clear();
        sub_dict->insert("three", 3);
        sub_dict->insert("four", 4);

        auto bar1 = table_2->get_object_with_primary_key(456);
        auto dict = bar1.get_dictionary("any");
        dict.clear();
        dict.insert("weight", 70);

        auto list_int = bar.get_list<Mixed>("ints");
        list_int.clear();
        list_int.add(3);
        list_int.add(4);
        tr_2->commit_and_continue_as_read();
    }

    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_upload_complete_or_client_stopped();
    session_1.wait_for_download_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();

    tr_1->advance_read();
    tr_2->advance_read();
    auto list = foo.get_list<Mixed>("any");
    CHECK_EQUAL(list.size(), 1);

    list = foo.get_list<Mixed>("any_list");
    CHECK_EQUAL(list.get_list(0)->size(), 2);
    CHECK_EQUAL(list.get_dictionary(1)->size(), 2);

    auto dict = foo1.get_dictionary("any");
    CHECK_EQUAL(dict.size(), 1);

    auto list_int = foo.get_list<Mixed>("ints");
    CHECK_EQUAL(list_int.size(), 4); // We should still have odd behavior for normal lists

    CHECK(compare_groups(*tr_1, *tr_2));
}

TEST(Sync_Dictionary_Links)
{
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    TEST_DIR(dir);
    fixtures::ClientServerFixture fixture{dir, test_context};
    fixture.start();

    Session session_1 = fixture.make_session(db_1, "/test");
    Session session_2 = fixture.make_session(db_2, "/test");

    // Test that we can transmit links.

    ColKey col_dict;

    write_transaction(db_1, [&](WriteTransaction& tr) {
        auto& g = tr.get_group();
        auto foos = g.add_table_with_primary_key("class_Foo", type_Int, "id");
        auto bars = g.add_table_with_primary_key("class_Bar", type_String, "id");
        col_dict = foos->add_column_dictionary(type_Mixed, "dict");

        auto foo = foos->create_object_with_primary_key(123);
        auto a = bars->create_object_with_primary_key("a");
        auto b = bars->create_object_with_primary_key("b");

        auto dict = foo.get_dictionary(col_dict);
        dict.insert("a", a);
        dict.insert("b", b);
    });

    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();

    {
        ReadTransaction tr{db_2};

        auto foos = tr.get_table("class_Foo");
        auto bars = tr.get_table("class_Bar");

        CHECK_EQUAL(foos->size(), 1);
        CHECK_EQUAL(bars->size(), 2);

        auto foo = foos->get_object_with_primary_key(123);
        auto a = bars->get_object_with_primary_key("a");
        auto b = bars->get_object_with_primary_key("b");

        auto dict = foo.get_dictionary(foos->get_column_key("dict"));
        CHECK_EQUAL(dict.size(), 2);

        auto dict_a = dict.get("a");
        auto dict_b = dict.get("b");
        CHECK(dict_a == Mixed{a.get_link()});
        CHECK(dict_b == Mixed{b.get_link()});
    }

    // Test that we can create tombstones for objects in dictionaries.

    write_transaction(db_1, [&](WriteTransaction& tr) {
        auto& g = tr.get_group();

        auto bars = g.get_table("class_Bar");
        auto a = bars->get_object_with_primary_key("a");
        a.invalidate();

        auto foos = g.get_table("class_Foo");
        auto foo = foos->get_object_with_primary_key(123);
        auto dict = foo.get_dictionary(col_dict);

        CHECK_EQUAL(dict.size(), 2);
        CHECK((*dict.find("a")).second.is_null());

        CHECK(dict.find("b") != dict.end());
    });

    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();

    {
        ReadTransaction tr{db_2};

        auto foos = tr.get_table("class_Foo");
        auto bars = tr.get_table("class_Bar");

        CHECK_EQUAL(foos->size(), 1);
        CHECK_EQUAL(bars->size(), 1);

        auto b = bars->get_object_with_primary_key("b");

        auto foo = foos->get_object_with_primary_key(123);
        auto dict = foo.get_dictionary(col_dict);

        CHECK_EQUAL(dict.size(), 2);
        CHECK((*dict.find("a")).second.is_null());

        CHECK(dict.find("b") != dict.end());
        CHECK((*dict.find("b")).second == Mixed{b.get_link()});
    }
}

TEST(Sync_Set)
{
    // Test replication and synchronization of Set values.

    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    TEST_DIR(dir);
    fixtures::ClientServerFixture fixture{dir, test_context};
    fixture.start();

    Session session_1 = fixture.make_session(db_1, "/test");
    Session session_2 = fixture.make_session(db_2, "/test");

    ColKey col_ints, col_strings, col_mixeds;
    {
        WriteTransaction wt{db_1};
        auto t = wt.get_group().add_table_with_primary_key("class_Foo", type_Int, "pk");
        col_ints = t->add_column_set(type_Int, "ints");
        col_strings = t->add_column_set(type_String, "strings");
        col_mixeds = t->add_column_set(type_Mixed, "mixeds");

        auto obj = t->create_object_with_primary_key(0);

        auto ints = obj.get_set<int64_t>(col_ints);
        auto strings = obj.get_set<StringData>(col_strings);
        auto mixeds = obj.get_set<Mixed>(col_mixeds);

        ints.insert(123);
        ints.insert(456);
        ints.insert(789);
        ints.insert(123);
        ints.insert(456);
        ints.insert(789);

        CHECK_EQUAL(ints.size(), 3);
        CHECK_EQUAL(ints.find(123), 0);
        CHECK_EQUAL(ints.find(456), 1);
        CHECK_EQUAL(ints.find(789), 2);

        strings.insert("a");
        strings.insert("b");
        strings.insert("c");
        strings.insert("a");
        strings.insert("b");
        strings.insert("c");

        CHECK_EQUAL(strings.size(), 3);
        CHECK_EQUAL(strings.find("a"), 0);
        CHECK_EQUAL(strings.find("b"), 1);
        CHECK_EQUAL(strings.find("c"), 2);

        mixeds.insert(Mixed{123});
        mixeds.insert(Mixed{"a"});
        mixeds.insert(Mixed{456.0});
        mixeds.insert(Mixed{123});
        mixeds.insert(Mixed{"a"});
        mixeds.insert(Mixed{456.0});

        CHECK_EQUAL(mixeds.size(), 3);
        CHECK_EQUAL(mixeds.find(123), 0);
        CHECK_EQUAL(mixeds.find(456.0), 1);
        CHECK_EQUAL(mixeds.find("a"), 2);

        wt.commit();
    }

    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();

    // Create a conflict. Session 1 should lose, because it has a lower peer ID.
    write_transaction(db_1, [=](WriteTransaction& wt) {
        auto t = wt.get_table("class_Foo");
        auto obj = t->get_object_with_primary_key(0);

        auto ints = obj.get_set<int64_t>(col_ints);
        ints.insert(999);
    });

    write_transaction(db_2, [=](WriteTransaction& wt) {
        auto t = wt.get_table("class_Foo");
        auto obj = t->get_object_with_primary_key(0);

        auto ints = obj.get_set<int64_t>(col_ints);
        ints.insert(999);
        ints.erase(999);
    });

    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_upload_complete_or_client_stopped();
    session_1.wait_for_download_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();

    {
        ReadTransaction read_1{db_1};
        ReadTransaction read_2{db_2};
        CHECK(compare_groups(read_1, read_2));
    }

    write_transaction(db_1, [=](WriteTransaction& wt) {
        auto t = wt.get_table("class_Foo");
        auto obj = t->get_object_with_primary_key(0);
        auto ints = obj.get_set<int64_t>(col_ints);
        ints.clear();
    });

    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();

    {
        ReadTransaction read_1{db_1};
        ReadTransaction read_2{db_2};
        CHECK(compare_groups(read_1, read_2));
    }
}

TEST(Sync_BundledBarqFile)
{
    TEST_CLIENT_DB(db);
    SHARED_GROUP_TEST_PATH(path);

    TEST_DIR(dir);
    fixtures::ClientServerFixture fixture{dir, test_context};
    fixture.start();

    write_transaction(db, [](WriteTransaction& tr) {
        auto foos = tr.get_group().add_table_with_primary_key("class_Foo", type_Int, "id");
        foos->create_object_with_primary_key(123);
    });

    // We cannot write out file if changes are not synced to server
    CHECK_THROW_ANY(db->write_copy(path.c_str(), nullptr));

    Session session = fixture.make_bound_session(db);
    session.wait_for_upload_complete_or_client_stopped();
    session.wait_for_download_complete_or_client_stopped();

    // Now we can
    db->write_copy(path.c_str(), nullptr);
}

TEST(Sync_UpgradeToClientHistory)
{
    DBOptions options;
    options.logger = test_context.logger;
    SHARED_GROUP_TEST_PATH(db_1_path);
    SHARED_GROUP_TEST_PATH(db_2_path);
    auto db_1 = DB::create(make_in_barq_history(), db_1_path, options);
    auto db_2 = DB::create(make_in_barq_history(), db_2_path, options);
    {
        auto tr = db_1->start_write();

        auto embedded = tr->add_table("class_Embedded", Table::Type::Embedded);
        auto col_float = embedded->add_column(type_Float, "float");
        auto col_additional = embedded->add_column_dictionary(*embedded, "additional");

        auto baa_table = tr->add_table_with_primary_key("class_Baa", type_Int, "_id");
        auto col_list = baa_table->add_column_list(type_Int, "list");
        auto col_set = baa_table->add_column_set(type_Int, "set");
        auto col_dict = baa_table->add_column_dictionary(type_Int, "dictionary");
        auto col_child = baa_table->add_column(*embedded, "child");

        auto foos = tr->add_table_with_primary_key("class_Foo", type_String, "_id");
        auto col_str = foos->add_column(type_String, "str");
        auto col_children = foos->add_column_list(*embedded, "children");

        auto foobaa_table = tr->add_table_with_primary_key("class_FooBaa", type_ObjectId, "_id");
        auto col_time = foobaa_table->add_column(type_Timestamp, "time");

        auto col_link = baa_table->add_column(*foos, "link");

        auto foo = foos->create_object_with_primary_key("123").set(col_str, "Hello");
        auto children = foo.get_linklist(col_children);
        children.create_and_insert_linked_object(0);
        auto baa = baa_table->create_object_with_primary_key(999).set(col_link, foo.get_key());
        auto obj = baa.create_and_set_linked_object(col_child);
        obj.set(col_float, 42.f);
        auto additional = obj.get_dictionary(col_additional);
        additional.create_and_insert_linked_object("One").set(col_float, 1.f);
        additional.create_and_insert_linked_object("Two").set(col_float, 2.f);
        additional.create_and_insert_linked_object("Three").set(col_float, 3.f);

        auto list = baa.get_list<Int>(col_list);
        list.add(1);
        list.add(0);
        list.add(2);
        list.add(3);
        list.set(1, 5);
        list.remove(1);
        auto set = baa.get_set<Int>(col_set);
        set.insert(4);
        set.insert(2);
        set.insert(5);
        set.insert(6);
        set.erase(2);
        auto dict = baa.get_dictionary(col_dict);
        dict.insert("key6", 6);
        dict.insert("key7", 7);
        dict.insert("key8", 8);
        dict.insert("key9", 9);
        dict.erase("key6");

        for (int i = 0; i < 100; i++) {
            foobaa_table->create_object_with_primary_key(ObjectId::gen()).set(col_time, Timestamp(::time(nullptr), i));
        }

        tr->commit();
    }
    {
        auto tr = db_2->start_write();
        auto baa_table = tr->add_table_with_primary_key("class_Baa", type_Int, "_id");
        auto foos = tr->add_table_with_primary_key("class_Foo", type_String, "_id");
        auto col_str = foos->add_column(type_String, "str");
        auto col_link = baa_table->add_column(*foos, "link");

        auto foo = foos->create_object_with_primary_key("123").set(col_str, "Goodbye");
        baa_table->create_object_with_primary_key(888).set(col_link, foo.get_key());

        tr->commit();
    }

    db_1->create_new_history(make_client_replication());
    db_2->create_new_history(make_client_replication());

    TEST_DIR(dir);
    fixtures::ClientServerFixture fixture{dir, test_context};
    fixture.start();

    Session session_1 = fixture.make_session(db_1, "/test");
    Session session_2 = fixture.make_session(db_2, "/test");

    write_transaction(db_1, [](WriteTransaction& tr) {
        auto foos = tr.get_group().get_table("class_Foo");
        foos->create_object_with_primary_key("456");
    });
    session_1.wait_for_upload_complete_or_client_stopped();
    session_2.wait_for_upload_complete_or_client_stopped();
    session_1.wait_for_download_complete_or_client_stopped();
    session_2.wait_for_download_complete_or_client_stopped();

    // db_2->start_read()->to_json(std::cout);
}

// This test is extracted from ClientReset_ThreeClients
// because it uncovers a bug in how MSVC 2019 compiles
// things in Changeset::get_key()
TEST(Sync_MergeStringPrimaryKey)
{
    TEST_DIR(dir_1); // The server.
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);
    TEST_DIR(metadata_dir_1);
    TEST_DIR(metadata_dir_2);

    const std::string server_path = "/data";

    std::string real_path_1, real_path_2;

    auto create_schema = [&](Transaction& group) {
        TableRef table_0 = group.add_table_with_primary_key("class_table_0", type_Int, "id");
        table_0->add_column(type_Int, "int");
        table_0->add_column(type_Bool, "bool");
        table_0->add_column(type_Float, "float");
        table_0->add_column(type_Double, "double");
        table_0->add_column(type_Timestamp, "timestamp");

        TableRef table_1 = group.add_table_with_primary_key("class_table_1", type_Int, "pk_int");
        table_1->add_column(type_String, "String");

        TableRef table_2 = group.add_table_with_primary_key("class_table_2", type_String, "pk_string");
        table_2->add_column_list(type_String, "array_string");
    };

    // First we make changesets. Then we upload them.
    {
        ClientServerFixture fixture(dir_1, test_context);
        fixture.start();
        real_path_1 = fixture.map_virtual_to_real_path(server_path);

        {
            WriteTransaction wt{db_1};
            create_schema(wt);
            wt.commit();
        }
        {
            WriteTransaction wt{db_2};
            create_schema(wt);

            TableRef table_2 = wt.get_table("class_table_2");
            auto col = table_2->get_column_key("array_string");
            auto list_string = table_2->create_object_with_primary_key("aaa").get_list<String>(col);
            list_string.add("a");
            list_string.add("b");

            wt.commit();
        }

        Session session_1 = fixture.make_bound_session(db_1, server_path);
        Session session_2 = fixture.make_bound_session(db_2, server_path);

        session_1.wait_for_upload_complete_or_client_stopped();
        session_2.wait_for_upload_complete_or_client_stopped();
        // Download completion is not important.
    }
}

TEST(Sync_DifferentUsersMultiplexing)
{
    ClientServerFixture::Config fixture_config;
    fixture_config.one_connection_per_session = false;

    TEST_DIR(server_dir);
    ClientServerFixture fixture(server_dir, test_context, std::move(fixture_config));

    struct SessionBundle {
        test_util::DBTestPathGuard path_guard;
        DBRef db;
        Session sess;

        SessionBundle(unit_test::TestContext& ctx, ClientServerFixture& fixture, std::string name,
                      std::string signed_token, std::string user_id)
            : path_guard(barq::test_util::get_test_path(ctx.get_test_name(), "." + name + ".barq"))
            , db(DB::create(make_client_replication(), path_guard))
        {
            Session::Config config;
            config.signed_user_token = signed_token;
            config.user_id = user_id;
            sess = fixture.make_bound_session(db, "/test", std::move(config));
            sess.wait_for_download_complete_or_client_stopped();
        }
    };

    fixture.start();

    SessionBundle user_1_sess_1(test_context, fixture, "user_1_db_1", g_user_0_token, "user_0");
    SessionBundle user_2_sess_1(test_context, fixture, "user_2_db_1", g_user_1_token, "user_1");
    SessionBundle user_1_sess_2(test_context, fixture, "user_1_db_2", g_user_0_token, "user_0");
    SessionBundle user_2_sess_2(test_context, fixture, "user_2_db_2", g_user_1_token, "user_1");

    CHECK_EQUAL(user_1_sess_1.sess.get_barq_connection_id(), user_1_sess_2.sess.get_barq_connection_id());
    CHECK_EQUAL(user_2_sess_1.sess.get_barq_connection_id(), user_2_sess_2.sess.get_barq_connection_id());
    CHECK_NOT_EQUAL(user_1_sess_1.sess.get_barq_connection_id(), user_2_sess_1.sess.get_barq_connection_id());
    CHECK_NOT_EQUAL(user_1_sess_2.sess.get_barq_connection_id(), user_2_sess_2.sess.get_barq_connection_id());
}

namespace {

void create_flx_isolation_schema(DBRef db, int64_t order_id, const char* owner_id, bool add_catalog_row)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto orders = wt.get_group().add_table_with_primary_key("class_Order", type_Int, "id");
        orders->add_column(type_String, "owner_id");
        orders->add_column(type_String, "item");

        auto catalog = wt.get_group().add_table_with_primary_key("class_Catalog", type_Int, "id");
        catalog->add_column(type_String, "name");

        if (owner_id) {
            auto order = orders->create_object_with_primary_key(order_id);
            order.set("owner_id", std::string(owner_id));
            order.set("item", util::format("item_%1", order_id));
        }
        if (add_catalog_row) {
            auto item = catalog->create_object_with_primary_key(10);
            item.set("name", "shared");
        }
    });
}

SubscriptionSet subscribe_to_flx_isolation_tables(DBRef db, const SubscriptionStoreRef& sub_store)
{
    auto rt = db->start_read();
    auto mut = sub_store->get_latest().make_mutable_copy();
    mut.insert_or_assign("orders", rt->get_table("class_Order")->where());
    mut.insert_or_assign("catalog", rt->get_table("class_Catalog")->where());
    return mut.commit();
}

SubscriptionSet subscribe_to_flx_isolation_tables_with_category(DBRef db, const SubscriptionStoreRef& sub_store)
{
    auto rt = db->start_read();
    auto mut = sub_store->get_latest().make_mutable_copy();
    mut.insert_or_assign("orders", rt->get_table("class_Order")->where());
    mut.insert_or_assign("catalog", rt->get_table("class_Catalog")->where());
    mut.insert_or_assign("category", rt->get_table("class_Category")->where());
    return mut.commit();
}

SubscriptionSet set_flx_isolation_subscriptions(DBRef db, const SubscriptionStoreRef& sub_store, bool include_orders,
                                                bool include_catalog)
{
    auto rt = db->start_read();
    auto mut = sub_store->get_latest().make_mutable_copy();
    mut.clear();
    if (include_orders) {
        mut.insert_or_assign("orders", rt->get_table("class_Order")->where());
    }
    if (include_catalog) {
        mut.insert_or_assign("catalog", rt->get_table("class_Catalog")->where());
    }
    return mut.commit();
}

void upsert_order_owner(DBRef db, int64_t order_id, const char* owner_id)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto orders = wt.get_group().get_table("class_Order");
        auto order = orders->get_object_with_primary_key(order_id);
        if (!order)
            order = orders->create_object_with_primary_key(order_id);
        order.set("owner_id", std::string(owner_id));
        order.set("item", util::format("item_%1", order_id));
    });
}

void update_order_item_and_create_order(DBRef db, int64_t allowed_order_id, const char* item, int64_t denied_order_id,
                                        const char* denied_owner_id)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto orders = wt.get_group().get_table("class_Order");
        auto allowed = orders->get_object_with_primary_key(allowed_order_id);
        if (!allowed) {
            throw std::runtime_error(util::format("Allowed order %1 not found", allowed_order_id));
        }
        allowed.set("item", std::string(item));

        auto denied = orders->get_object_with_primary_key(denied_order_id);
        if (!denied) {
            denied = orders->create_object_with_primary_key(denied_order_id);
        }
        denied.set("owner_id", std::string(denied_owner_id));
        denied.set("item", util::format("item_%1", denied_order_id));
    });
}

void add_flx_orders(DBRef db, int64_t first_order_id, int64_t last_order_id, const char* owner_id)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto orders = wt.get_group().get_table("class_Order");
        for (int64_t order_id = first_order_id; order_id <= last_order_id; ++order_id) {
            auto order = orders->create_object_with_primary_key(order_id);
            order.set("owner_id", std::string(owner_id));
            order.set("item", util::format("item_%1", order_id));
        }
    });
}

void delete_flx_order(DBRef db, int64_t order_id)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto orders = wt.get_group().get_table("class_Order");
        auto order = orders->get_object_with_primary_key(order_id);
        if (order) {
            order.remove();
        }
    });
}

void add_flx_catalog_items(DBRef db, int64_t first_item_id, int64_t last_item_id)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto catalog = wt.get_group().get_table("class_Catalog");
        for (int64_t item_id = first_item_id; item_id <= last_item_id; ++item_id) {
            auto item = catalog->create_object_with_primary_key(item_id);
            item.set("name", util::format("shared_%1", item_id));
        }
    });
}

void set_flx_catalog_name(DBRef db, int64_t item_id, const char* name)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto catalog = wt.get_group().get_table("class_Catalog");
        auto item = catalog->get_object_with_primary_key(item_id);
        if (!item)
            item = catalog->create_object_with_primary_key(item_id);
        item.set("name", std::string(name));
    });
}

void delete_flx_catalog_item(DBRef db, int64_t item_id)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto catalog = wt.get_group().get_table("class_Catalog");
        auto item = catalog->get_object_with_primary_key(item_id);
        if (item) {
            item.remove();
        }
    });
}

void add_flx_catalog_scores_schema(DBRef db, const std::vector<int64_t>& scores)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto catalog = wt.get_group().get_table("class_Catalog");
        auto scores_col = catalog->get_column_key("scores");
        if (!scores_col) {
            scores_col = catalog->add_column_list(type_Int, "scores");
        }

        auto item = catalog->get_object_with_primary_key(10);
        if (!item) {
            return;
        }

        auto list = item.get_list<int64_t>(scores_col);
        list.clear();
        for (int64_t score : scores) {
            list.add(score);
        }
    });
}

void set_flx_catalog_scores(DBRef db, const std::vector<int64_t>& scores)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto catalog = wt.get_group().get_table("class_Catalog");
        auto item = catalog->get_object_with_primary_key(10);
        auto list = item.get_list<int64_t>("scores");
        list.clear();
        for (int64_t score : scores) {
            list.add(score);
        }
    });
}

void add_flx_catalog_score_set_schema(DBRef db, const std::vector<int64_t>& scores)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto catalog = wt.get_group().get_table("class_Catalog");
        auto scores_col = catalog->get_column_key("score_set");
        if (!scores_col) {
            scores_col = catalog->add_column_set(type_Int, "score_set");
        }

        auto item = catalog->get_object_with_primary_key(10);
        if (!item) {
            return;
        }

        auto set = item.get_set<int64_t>(scores_col);
        set.clear();
        for (int64_t score : scores) {
            set.insert(score);
        }
    });
}

void set_flx_catalog_score_set(DBRef db, const std::vector<int64_t>& scores)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto catalog = wt.get_group().get_table("class_Catalog");
        auto item = catalog->get_object_with_primary_key(10);
        auto set = item.get_set<int64_t>("score_set");
        set.clear();
        for (int64_t score : scores) {
            set.insert(score);
        }
    });
}

void add_flx_catalog_note_dict_schema(DBRef db, const std::vector<std::pair<std::string, std::string>>& notes)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto catalog = wt.get_group().get_table("class_Catalog");
        auto notes_col = catalog->get_column_key("notes");
        if (!notes_col) {
            notes_col = catalog->add_column_dictionary(type_String, "notes");
        }

        auto item = catalog->get_object_with_primary_key(10);
        if (!item) {
            return;
        }

        auto dictionary = item.get_dictionary(notes_col);
        dictionary.clear();
        for (const auto& note : notes) {
            dictionary.insert(StringData(note.first), StringData(note.second));
        }
    });
}

void set_flx_catalog_note_dict(DBRef db, const std::vector<std::pair<std::string, std::string>>& notes)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto catalog = wt.get_group().get_table("class_Catalog");
        auto item = catalog->get_object_with_primary_key(10);
        auto dictionary = item.get_dictionary("notes");
        dictionary.clear();
        for (const auto& note : notes) {
            dictionary.insert(StringData(note.first), StringData(note.second));
        }
    });
}

void add_flx_catalog_category_schema(DBRef db, bool add_row)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto catalog = wt.get_group().get_table("class_Catalog");
        auto category = wt.get_group().get_table("class_Category");
        if (!category) {
            category = wt.get_group().add_table_with_primary_key("class_Category", type_Int, "id");
            category->add_column(type_String, "label");
        }
        else if (!category->get_column_key("label")) {
            category->add_column(type_String, "label");
        }

        auto category_col = catalog->get_column_key("category");
        if (!category_col) {
            category_col = catalog->add_column(*category, "category");
        }

        if (!add_row) {
            return;
        }

        auto target = category->get_object_with_primary_key(50);
        if (!target) {
            target = category->create_object_with_primary_key(50);
        }
        target.set("label", "tools");

        auto item = catalog->get_object_with_primary_key(10);
        if (item) {
            item.set<ObjKey>(category_col, target.get_key());
        }
    });
}

void set_flx_catalog_category_null(DBRef db)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto catalog = wt.get_group().get_table("class_Catalog");
        auto item = catalog->get_object_with_primary_key(10);
        item.set<ObjKey>(catalog->get_column_key("category"), ObjKey{});
    });
}

void add_flx_catalog_category_collections_schema(DBRef db, bool add_rows)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto catalog = wt.get_group().get_table("class_Catalog");
        auto category = wt.get_group().get_table("class_Category");
        if (!category) {
            category = wt.get_group().add_table_with_primary_key("class_Category", type_Int, "id");
            category->add_column(type_String, "label");
        }
        else if (!category->get_column_key("label")) {
            category->add_column(type_String, "label");
        }

        auto list_col = catalog->get_column_key("category_list");
        if (!list_col) {
            list_col = catalog->add_column_list(*category, "category_list");
        }
        auto set_col = catalog->get_column_key("category_set");
        if (!set_col) {
            set_col = catalog->add_column_set(*category, "category_set");
        }
        auto dict_col = catalog->get_column_key("category_dict");
        if (!dict_col) {
            dict_col = catalog->add_column_dictionary(*category, "category_dict");
        }

        if (!add_rows) {
            return;
        }

        auto first = category->get_object_with_primary_key(50);
        if (!first) {
            first = category->create_object_with_primary_key(50);
        }
        first.set("label", "tools");

        auto second = category->get_object_with_primary_key(51);
        if (!second) {
            second = category->create_object_with_primary_key(51);
        }
        second.set("label", "books");

        auto item = catalog->get_object_with_primary_key(10);
        if (!item) {
            return;
        }

        auto list = item.get_linklist(list_col);
        list.clear();
        list.add(first.get_key());
        list.add(second.get_key());

        auto set = item.get_linkset(set_col);
        set.clear();
        set.insert(first.get_key());
        set.insert(second.get_key());

        auto dictionary = item.get_dictionary(dict_col);
        dictionary.clear();
        dictionary.insert("primary", first.get_link());
        dictionary.insert("secondary", second.get_link());
    });
}

void set_flx_catalog_category_collections_to_second(DBRef db)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto catalog = wt.get_group().get_table("class_Catalog");
        auto category = wt.get_group().get_table("class_Category");
        auto second = category->get_object_with_primary_key(51);
        auto item = catalog->get_object_with_primary_key(10);

        auto list = item.get_linklist("category_list");
        list.clear();
        list.add(second.get_key());

        auto set = item.get_linkset("category_set");
        set.clear();
        set.insert(second.get_key());

        auto dictionary = item.get_dictionary("category_dict");
        dictionary.clear();
        dictionary.insert("secondary", second.get_link());
    });
}

void add_flx_catalog_detail_schema(DBRef db, bool add_row)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto catalog = wt.get_group().get_table("class_Catalog");
        auto detail = wt.get_group().get_table("class_CatalogDetail");
        if (!detail) {
            detail = wt.get_group().add_table("class_CatalogDetail", Table::Type::Embedded);
            detail->add_column(type_String, "summary");
            detail->add_column(type_Int, "rating");
        }

        auto detail_col = catalog->get_column_key("detail");
        if (!detail_col) {
            detail_col = catalog->add_column(*detail, "detail");
        }

        if (!add_row) {
            return;
        }

        auto item = catalog->get_object_with_primary_key(10);
        if (!item) {
            return;
        }

        auto detail_obj = item.get_linked_object(detail_col);
        if (!detail_obj) {
            detail_obj = item.create_and_set_linked_object(detail_col);
        }
        detail_obj.set("summary", "seeded");
        detail_obj.set("rating", int64_t(5));
    });
}

void set_flx_catalog_detail_summary(DBRef db, const char* summary)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto catalog = wt.get_group().get_table("class_Catalog");
        auto item = catalog->get_object_with_primary_key(10);
        auto detail_col = catalog->get_column_key("detail");
        auto detail = item.get_linked_object(detail_col);
        if (!detail) {
            detail = item.create_and_set_linked_object(detail_col);
        }
        detail.set("summary", std::string(summary));
    });
}

void add_flx_catalog_detail_collections_schema(DBRef db, bool add_rows)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto catalog = wt.get_group().get_table("class_Catalog");
        auto detail = wt.get_group().get_table("class_CatalogDetailEntry");
        if (!detail) {
            detail = wt.get_group().add_table("class_CatalogDetailEntry", Table::Type::Embedded);
            detail->add_column(type_String, "summary");
            detail->add_column(type_Int, "rating");
        }

        auto list_col = catalog->get_column_key("detail_list");
        if (!list_col) {
            list_col = catalog->add_column_list(*detail, "detail_list");
        }
        auto dict_col = catalog->get_column_key("detail_dict");
        if (!dict_col) {
            dict_col = catalog->add_column_dictionary(*detail, "detail_dict");
        }

        if (!add_rows) {
            return;
        }

        auto item = catalog->get_object_with_primary_key(10);
        if (!item) {
            return;
        }

        auto list = item.get_linklist(list_col);
        list.clear();
        list.create_and_insert_linked_object(0).set_all("list_one", int64_t(1));
        list.create_and_insert_linked_object(1).set_all("list_two", int64_t(2));

        auto dictionary = item.get_dictionary(dict_col);
        dictionary.clear();
        dictionary.create_and_insert_linked_object("primary").set_all("dict_one", int64_t(3));
        dictionary.create_and_insert_linked_object("secondary").set_all("dict_two", int64_t(4));
    });
}

void set_flx_catalog_detail_collections_hacked(DBRef db)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto catalog = wt.get_group().get_table("class_Catalog");
        auto item = catalog->get_object_with_primary_key(10);

        auto list = item.get_linklist("detail_list");
        list.clear();
        list.create_and_insert_linked_object(0).set_all("hacked_list", int64_t(99));

        auto dictionary = item.get_dictionary("detail_dict");
        dictionary.clear();
        dictionary.create_and_insert_linked_object("bad").set_all("hacked_dict", int64_t(100));
    });
}

void add_flx_catalog_mixed_schema(DBRef db, bool add_values)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto catalog = wt.get_group().get_table("class_Catalog");
        if (!catalog->get_column_key("mixed_value")) {
            catalog->add_column(type_Mixed, "mixed_value", true);
        }
        if (!catalog->get_column_key("mixed_list")) {
            catalog->add_column_list(type_Mixed, "mixed_list");
        }
        if (!catalog->get_column_key("mixed_set")) {
            catalog->add_column_set(type_Mixed, "mixed_set");
        }
        if (!catalog->get_column_key("mixed_dict")) {
            catalog->add_column_dictionary(type_Mixed, "mixed_dict");
        }

        if (!add_values) {
            return;
        }

        auto item = catalog->get_object_with_primary_key(10);
        if (!item) {
            return;
        }

        item.set("mixed_value", Mixed{int64_t(42)});

        auto list = item.get_list<Mixed>("mixed_list");
        list.clear();
        list.insert(0, Mixed{"alpha"});
        list.insert(1, Mixed{int64_t(7)});
        list.insert(2, Mixed{true});

        auto set = item.get_set<Mixed>("mixed_set");
        set.clear();
        set.insert(Mixed{int64_t(3)});
        set.insert(Mixed{"set_value"});

        auto dictionary = item.get_dictionary("mixed_dict");
        dictionary.clear();
        dictionary.insert("name", Mixed{"dict_value"});
        dictionary.insert("count", Mixed{int64_t(9)});
    });
}

void set_flx_catalog_mixed_hacked(DBRef db)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto catalog = wt.get_group().get_table("class_Catalog");
        auto item = catalog->get_object_with_primary_key(10);

        item.set("mixed_value", Mixed{"hacked"});

        auto list = item.get_list<Mixed>("mixed_list");
        list.clear();
        list.insert(0, Mixed{int64_t(99)});

        auto set = item.get_set<Mixed>("mixed_set");
        set.clear();
        set.insert(Mixed{"bad"});

        auto dictionary = item.get_dictionary("mixed_dict");
        dictionary.clear();
        dictionary.insert("bad", Mixed{int64_t(100)});
    });
}

void add_flx_catalog_mixed_link_schema(DBRef db, bool add_values)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto catalog = wt.get_group().get_table("class_Catalog");
        auto category = wt.get_group().get_table("class_Category");
        if (!category) {
            category = wt.get_group().add_table_with_primary_key("class_Category", type_Int, "id");
            category->add_column(type_String, "label");
        }
        else if (!category->get_column_key("label")) {
            category->add_column(type_String, "label");
        }

        if (!catalog->get_column_key("mixed_link")) {
            catalog->add_column(type_Mixed, "mixed_link", true);
        }
        if (!catalog->get_column_key("mixed_link_list")) {
            catalog->add_column_list(type_Mixed, "mixed_link_list");
        }
        if (!catalog->get_column_key("mixed_link_set")) {
            catalog->add_column_set(type_Mixed, "mixed_link_set");
        }
        if (!catalog->get_column_key("mixed_link_dict")) {
            catalog->add_column_dictionary(type_Mixed, "mixed_link_dict");
        }

        if (!add_values) {
            return;
        }

        auto first = category->get_object_with_primary_key(50);
        if (!first) {
            first = category->create_object_with_primary_key(50);
        }
        first.set("label", "tools");

        auto second = category->get_object_with_primary_key(51);
        if (!second) {
            second = category->create_object_with_primary_key(51);
        }
        second.set("label", "books");

        auto item = catalog->get_object_with_primary_key(10);
        if (!item) {
            return;
        }

        item.set("mixed_link", Mixed{first.get_link()});

        auto list = item.get_list<Mixed>("mixed_link_list");
        list.clear();
        list.insert(0, Mixed{first.get_link()});
        list.insert(1, Mixed{second.get_link()});

        auto set = item.get_set<Mixed>("mixed_link_set");
        set.clear();
        set.insert(Mixed{first.get_link()});
        set.insert(Mixed{second.get_link()});

        auto dictionary = item.get_dictionary("mixed_link_dict");
        dictionary.clear();
        dictionary.insert("primary", Mixed{first.get_link()});
        dictionary.insert("secondary", Mixed{second.get_link()});
    });
}

void set_flx_catalog_mixed_links_to_second(DBRef db)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto catalog = wt.get_group().get_table("class_Catalog");
        auto category = wt.get_group().get_table("class_Category");
        auto second = category->get_object_with_primary_key(51);
        auto item = catalog->get_object_with_primary_key(10);

        item.set("mixed_link", Mixed{second.get_link()});

        auto list = item.get_list<Mixed>("mixed_link_list");
        list.clear();
        list.insert(0, Mixed{second.get_link()});

        auto set = item.get_set<Mixed>("mixed_link_set");
        set.clear();
        set.insert(Mixed{second.get_link()});

        auto dictionary = item.get_dictionary("mixed_link_dict");
        dictionary.clear();
        dictionary.insert("bad", Mixed{second.get_link()});
    });
}

void add_flx_private_schema(DBRef db)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto table = wt.get_group().get_table("class_Private");
        if (table) {
            return;
        }
        table = wt.get_group().add_table_with_primary_key("class_Private", type_Int, "id");
        table->add_column(type_String, "value");
    });
}

SubscriptionSet subscribe_to_flx_private_table(DBRef db, const SubscriptionStoreRef& sub_store)
{
    auto rt = db->start_read();
    auto table = rt->get_table("class_Private");
    auto mut = sub_store->get_latest().make_mutable_copy();
    mut.insert_or_assign("private", table->where());
    return mut.commit();
}

void create_flx_person_schema(DBRef db)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto people = wt.get_group().add_table_with_primary_key("class_Person", type_Int, "id");
        people->add_column(type_Int, "age");
    });
}

SubscriptionSet subscribe_to_adult_people(DBRef db, const SubscriptionStoreRef& sub_store)
{
    auto rt = db->start_read();
    auto people = rt->get_table("class_Person");
    auto mut = sub_store->get_latest().make_mutable_copy();
    mut.insert_or_assign("adults", people->where().greater(people->get_column_key("age"), int64_t(18)));
    return mut.commit();
}

void replace_subscription_query(unit_test::TestContext& test_context, DBRef db, int64_t version, const char* query)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto sub_sets = wt.get_group().get_table("flx_subscription_sets");
        CHECK(sub_sets);
        auto sub_set = sub_sets->get_object_with_primary_key(version);
        CHECK(sub_set);
        auto subscriptions = sub_set.get_linklist("subscriptions");
        CHECK_EQUAL(subscriptions.size(), 1);
        subscriptions.get_object(0).set("query", std::string(query));
    });
}

void upsert_flx_person(DBRef db, int64_t person_id, int64_t age)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto people = wt.get_group().get_table("class_Person");
        auto person = people->get_object_with_primary_key(person_id);
        if (!person)
            person = people->create_object_with_primary_key(person_id);
        person.set("age", age);
    });
}

bool wait_for_subscription_state(SubscriptionSet& sub_set, SubscriptionSet::State state)
{
    for (size_t i = 0; i < 500; ++i) {
        sub_set.refresh();
        if (sub_set.state() == state)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return false;
}

void upsert_flx_private_object(DBRef db, int64_t object_id, const char* value)
{
    write_transaction(db, [&](WriteTransaction& wt) {
        auto table = wt.get_group().get_table("class_Private");
        auto obj = table->get_object_with_primary_key(object_id);
        if (!obj)
            obj = table->create_object_with_primary_key(object_id);
        obj.set("value", std::string(value));
    });
}

void check_order_visibility(unit_test::TestContext& test_context, DBRef db, bool has_order_1, bool has_order_2)
{
    auto rt = db->start_read();
    auto orders = rt->get_table("class_Order");
    CHECK_EQUAL(bool(orders->get_object_with_primary_key(1)), has_order_1);
    CHECK_EQUAL(bool(orders->get_object_with_primary_key(2)), has_order_2);
    CHECK_EQUAL(orders->size(), size_t(has_order_1) + size_t(has_order_2));
}

void check_catalog_visible(unit_test::TestContext& test_context, DBRef db)
{
    auto rt = db->start_read();
    auto catalog = rt->get_table("class_Catalog");
    CHECK_EQUAL(catalog->size(), 1);
    CHECK(catalog->get_object_with_primary_key(10));
}

void check_catalog_count(unit_test::TestContext& test_context, DBRef db, std::size_t count)
{
    auto rt = db->start_read();
    auto catalog = rt->get_table("class_Catalog");
    CHECK_EQUAL(catalog->size(), count);
}

void check_catalog_name(unit_test::TestContext& test_context, DBRef db, int64_t item_id, const char* name)
{
    auto rt = db->start_read();
    auto catalog = rt->get_table("class_Catalog");
    auto item = catalog->get_object_with_primary_key(item_id);
    CHECK(item);
    CHECK_EQUAL(item.get<StringData>("name"), StringData(name));
}

void check_catalog_scores(unit_test::TestContext& test_context, DBRef db, const std::vector<int64_t>& expected)
{
    auto rt = db->start_read();
    auto catalog = rt->get_table("class_Catalog");
    auto item = catalog->get_object_with_primary_key(10);
    CHECK(item);
    auto list = item.get_list<int64_t>("scores");
    CHECK_EQUAL(list.size(), expected.size());
    for (size_t i = 0; i < expected.size() && i < list.size(); ++i) {
        CHECK_EQUAL(list.get(i), expected[i]);
    }
}

void check_catalog_score_set(unit_test::TestContext& test_context, DBRef db, const std::vector<int64_t>& expected)
{
    auto rt = db->start_read();
    auto catalog = rt->get_table("class_Catalog");
    auto item = catalog->get_object_with_primary_key(10);
    CHECK(item);
    auto set = item.get_set<int64_t>("score_set");
    CHECK_EQUAL(set.size(), expected.size());
    for (int64_t score : expected) {
        CHECK_NOT_EQUAL(set.find(score), npos);
    }
}

void check_catalog_note_dict(unit_test::TestContext& test_context, DBRef db,
                             const std::vector<std::pair<std::string, std::string>>& expected)
{
    auto rt = db->start_read();
    auto catalog = rt->get_table("class_Catalog");
    auto item = catalog->get_object_with_primary_key(10);
    CHECK(item);
    auto dictionary = item.get_dictionary("notes");
    CHECK_EQUAL(dictionary.size(), expected.size());
    for (const auto& note : expected) {
        auto value = dictionary.try_get(StringData(note.first));
        CHECK(value);
        if (value) {
            CHECK_EQUAL(value->get<StringData>(), StringData(note.second));
        }
    }
}

void check_catalog_category_label(unit_test::TestContext& test_context, DBRef db, const char* label)
{
    auto rt = db->start_read();
    auto catalog = rt->get_table("class_Catalog");
    auto item = catalog->get_object_with_primary_key(10);
    CHECK(item);
    auto category = item.get_linked_object("category");
    CHECK(category);
    if (category) {
        CHECK_EQUAL(category.get<StringData>("label"), StringData(label));
    }
}

void check_catalog_category_unresolved(unit_test::TestContext& test_context, DBRef db)
{
    auto rt = db->start_read();
    auto catalog = rt->get_table("class_Catalog");
    auto category = rt->get_table("class_Category");
    CHECK(category);
    CHECK_EQUAL(category->size(), 0);
    CHECK_EQUAL(category->nb_unresolved(), 1);

    auto item = catalog->get_object_with_primary_key(10);
    CHECK(item);
    auto category_col = catalog->get_column_key("category");
    CHECK(category_col);
    CHECK(item.is_unresolved(category_col));
}

void check_catalog_category_collections(unit_test::TestContext& test_context, DBRef db)
{
    auto rt = db->start_read();
    auto catalog = rt->get_table("class_Catalog");
    auto category = rt->get_table("class_Category");
    auto item = catalog->get_object_with_primary_key(10);
    CHECK(item);

    auto first = category->get_object_with_primary_key(50);
    auto second = category->get_object_with_primary_key(51);
    CHECK(first);
    CHECK(second);

    auto list = item.get_linklist("category_list");
    CHECK_EQUAL(list.size(), 2);
    if (list.size() == 2) {
        CHECK_EQUAL(category->get_object(list.get(0)).get<StringData>("label"), StringData("tools"));
        CHECK_EQUAL(category->get_object(list.get(1)).get<StringData>("label"), StringData("books"));
    }

    auto set = item.get_linkset("category_set");
    CHECK_EQUAL(set.size(), 2);
    CHECK_NOT_EQUAL(set.find(first.get_key()), npos);
    CHECK_NOT_EQUAL(set.find(second.get_key()), npos);

    auto dictionary = item.get_dictionary("category_dict");
    CHECK_EQUAL(dictionary.size(), 2);
    auto primary = dictionary.try_get("primary");
    auto secondary = dictionary.try_get("secondary");
    CHECK(primary);
    CHECK(secondary);
    if (primary) {
        CHECK_EQUAL(category->get_object(primary->get<ObjKey>()).get<StringData>("label"), StringData("tools"));
    }
    if (secondary) {
        CHECK_EQUAL(category->get_object(secondary->get<ObjKey>()).get<StringData>("label"), StringData("books"));
    }
}

void check_catalog_detail(unit_test::TestContext& test_context, DBRef db, const char* summary, int64_t rating)
{
    auto rt = db->start_read();
    auto catalog = rt->get_table("class_Catalog");
    auto item = catalog->get_object_with_primary_key(10);
    CHECK(item);
    auto detail = item.get_linked_object("detail");
    CHECK(detail);
    if (detail) {
        CHECK_EQUAL(detail.get<StringData>("summary"), StringData(summary));
        CHECK_EQUAL(detail.get<int64_t>("rating"), rating);
    }
}

void check_catalog_detail_collections(unit_test::TestContext& test_context, DBRef db)
{
    auto rt = db->start_read();
    auto catalog = rt->get_table("class_Catalog");
    auto item = catalog->get_object_with_primary_key(10);
    CHECK(item);

    auto list = item.get_linklist("detail_list");
    CHECK_EQUAL(list.size(), 2);
    if (list.size() == 2) {
        auto first = list.get_object(0);
        auto second = list.get_object(1);
        CHECK_EQUAL(first.get<StringData>("summary"), StringData("list_one"));
        CHECK_EQUAL(first.get<int64_t>("rating"), 1);
        CHECK_EQUAL(second.get<StringData>("summary"), StringData("list_two"));
        CHECK_EQUAL(second.get<int64_t>("rating"), 2);
    }

    auto dictionary = item.get_dictionary("detail_dict");
    CHECK_EQUAL(dictionary.size(), 2);
    auto primary = dictionary.get_object("primary");
    auto secondary = dictionary.get_object("secondary");
    CHECK(primary);
    CHECK(secondary);
    if (primary) {
        CHECK_EQUAL(primary.get<StringData>("summary"), StringData("dict_one"));
        CHECK_EQUAL(primary.get<int64_t>("rating"), 3);
    }
    if (secondary) {
        CHECK_EQUAL(secondary.get<StringData>("summary"), StringData("dict_two"));
        CHECK_EQUAL(secondary.get<int64_t>("rating"), 4);
    }
}

void check_catalog_mixed_values(unit_test::TestContext& test_context, DBRef db)
{
    auto rt = db->start_read();
    auto catalog = rt->get_table("class_Catalog");
    auto item = catalog->get_object_with_primary_key(10);
    CHECK(item);

    CHECK_EQUAL(item.get<Mixed>("mixed_value"), Mixed{int64_t(42)});

    auto list = item.get_list<Mixed>("mixed_list");
    CHECK_EQUAL(list.size(), 3);
    if (list.size() == 3) {
        CHECK_EQUAL(list.get(0), Mixed{"alpha"});
        CHECK_EQUAL(list.get(1), Mixed{int64_t(7)});
        CHECK_EQUAL(list.get(2), Mixed{true});
    }

    auto set = item.get_set<Mixed>("mixed_set");
    CHECK_EQUAL(set.size(), 2);
    CHECK_NOT_EQUAL(set.find(Mixed{int64_t(3)}), npos);
    CHECK_NOT_EQUAL(set.find(Mixed{"set_value"}), npos);

    auto dictionary = item.get_dictionary("mixed_dict");
    CHECK_EQUAL(dictionary.size(), 2);
    auto name = dictionary.try_get("name");
    auto count = dictionary.try_get("count");
    CHECK(name);
    CHECK(count);
    if (name) {
        CHECK_EQUAL(*name, Mixed{"dict_value"});
    }
    if (count) {
        CHECK_EQUAL(*count, Mixed{int64_t(9)});
    }
}

void check_mixed_link_label(unit_test::TestContext& test_context, ConstTableRef category, Mixed value,
                            const char* expected_label)
{
    CHECK(value.is_type(type_TypedLink));
    if (!value.is_type(type_TypedLink)) {
        return;
    }

    ObjLink link = value.get<ObjLink>();
    CHECK_EQUAL(link.get_table_key(), category->get_key());
    Obj target = category->try_get_object(link.get_obj_key());
    CHECK(target);
    if (target) {
        CHECK_EQUAL(target.get<StringData>("label"), StringData(expected_label));
    }
}

void check_catalog_mixed_links(unit_test::TestContext& test_context, DBRef db)
{
    auto rt = db->start_read();
    auto catalog = rt->get_table("class_Catalog");
    auto category = rt->get_table("class_Category");
    auto item = catalog->get_object_with_primary_key(10);
    CHECK(item);
    CHECK(category);
    if (!item || !category) {
        return;
    }

    Obj first = category->get_object_with_primary_key(50);
    Obj second = category->get_object_with_primary_key(51);
    CHECK(first);
    CHECK(second);

    check_mixed_link_label(test_context, category, item.get<Mixed>("mixed_link"), "tools");

    auto list = item.get_list<Mixed>("mixed_link_list");
    CHECK_EQUAL(list.size(), 2);
    if (list.size() == 2) {
        check_mixed_link_label(test_context, category, list.get(0), "tools");
        check_mixed_link_label(test_context, category, list.get(1), "books");
    }

    auto set = item.get_set<Mixed>("mixed_link_set");
    CHECK_EQUAL(set.size(), 2);
    if (first) {
        CHECK_NOT_EQUAL(set.find(Mixed{first.get_link()}), npos);
    }
    if (second) {
        CHECK_NOT_EQUAL(set.find(Mixed{second.get_link()}), npos);
    }

    auto dictionary = item.get_dictionary("mixed_link_dict");
    CHECK_EQUAL(dictionary.size(), 2);
    auto primary = dictionary.try_get("primary");
    auto secondary = dictionary.try_get("secondary");
    CHECK(primary);
    CHECK(secondary);
    if (primary) {
        check_mixed_link_label(test_context, category, *primary, "tools");
    }
    if (secondary) {
        check_mixed_link_label(test_context, category, *secondary, "books");
    }
}

void check_mixed_link_pending(unit_test::TestContext& test_context, Mixed value)
{
    CHECK(value.is_null() || value.is_unresolved_link());
}

void check_catalog_mixed_links_pending(unit_test::TestContext& test_context, DBRef db)
{
    auto rt = db->start_read();
    auto catalog = rt->get_table("class_Catalog");
    auto category = rt->get_table("class_Category");
    auto item = catalog->get_object_with_primary_key(10);
    CHECK(item);
    CHECK(category);
    if (!item || !category) {
        return;
    }
    CHECK_EQUAL(category->size(), 0);
    CHECK_EQUAL(category->nb_unresolved(), 2);

    check_mixed_link_pending(test_context, item.get<Mixed>("mixed_link"));

    auto list = item.get_list<Mixed>("mixed_link_list");
    CHECK_EQUAL(list.size(), 2);
    for (size_t i = 0; i < list.size(); ++i) {
        check_mixed_link_pending(test_context, list.get(i));
    }

    auto set = item.get_set<Mixed>("mixed_link_set");
    CHECK_EQUAL(set.size(), 2);
    for (size_t i = 0; i < set.size(); ++i) {
        check_mixed_link_pending(test_context, set.get_any(i));
    }

    auto dictionary = item.get_dictionary("mixed_link_dict");
    CHECK_EQUAL(dictionary.size(), 2);
    auto primary = dictionary.try_get("primary");
    auto secondary = dictionary.try_get("secondary");
    CHECK(primary);
    CHECK(secondary);
    if (primary) {
        check_mixed_link_pending(test_context, *primary);
    }
    if (secondary) {
        check_mixed_link_pending(test_context, *secondary);
    }
}

void check_private_count(unit_test::TestContext& test_context, DBRef db, std::size_t count)
{
    auto rt = db->start_read();
    auto table = rt->get_table("class_Private");
    CHECK(table);
    CHECK_EQUAL(table->size(), count);
}

void check_person_count(unit_test::TestContext& test_context, DBRef db, std::size_t count)
{
    auto rt = db->start_read();
    auto table = rt->get_table("class_Person");
    CHECK(table);
    CHECK_EQUAL(table->size(), count);
}

void check_order_count(unit_test::TestContext& test_context, DBRef db, std::size_t count)
{
    auto rt = db->start_read();
    auto orders = rt->get_table("class_Order");
    CHECK_EQUAL(orders->size(), count);
}

void check_order_exists(unit_test::TestContext& test_context, DBRef db, int64_t order_id, bool exists)
{
    auto rt = db->start_read();
    auto orders = rt->get_table("class_Order");
    CHECK_EQUAL(bool(orders->get_object_with_primary_key(order_id)), exists);
}

void check_order_owner(unit_test::TestContext& test_context, DBRef db, int64_t order_id, const char* owner_id)
{
    auto rt = db->start_read();
    auto orders = rt->get_table("class_Order");
    auto order = orders->get_object_with_primary_key(order_id);
    if (!order) {
        CHECK(order);
        return;
    }
    CHECK_EQUAL(order.get<StringData>("owner_id"), StringData(owner_id));
}

void check_order_item(unit_test::TestContext& test_context, DBRef db, int64_t order_id, const char* item)
{
    auto rt = db->start_read();
    auto orders = rt->get_table("class_Order");
    auto order = orders->get_object_with_primary_key(order_id);
    if (!order) {
        CHECK(order);
        return;
    }
    CHECK_EQUAL(order.get<StringData>("item"), StringData(item));
}

void set_server_order_owner_directly(const std::string& server_path, int64_t order_id, const char* owner_id)
{
    TestServerHistoryContext context;
    _impl::ServerHistory history{context};
    DBRef server_db = DB::create(history, server_path);
    write_transaction(server_db, [&](WriteTransaction& wt) {
        auto orders = wt.get_group().get_table("class_Order");
        auto order = orders->get_object_with_primary_key(order_id);
        if (!order) {
            throw std::runtime_error(util::format("Server order %1 not found", order_id));
        }
        order.set("owner_id", std::string(owner_id));
    });
}

} // anonymous namespace

TEST(Sync_FLXOwnerRulesIsolateUsersAndHandleReconnect)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);
    TEST_CLIENT_DB(db_b);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    upsert_order_owner(seed_db, 2, "user_1");
    create_flx_isolation_schema(db_a, 0, nullptr, false);
    create_flx_isolation_schema(db_b, 0, nullptr, false);

    auto sub_store_a = SubscriptionStore::create(db_a);
    auto sub_store_b = SubscriptionStore::create(db_b);
    subscribe_to_flx_isolation_tables(db_a, sub_store_a);
    subscribe_to_flx_isolation_tables(db_b, sub_store_b);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(3, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    Session::Config config_a;
    config_a.signed_user_token = g_user_0_path_test_token;
    config_a.user_id = "user_0";

    util::Optional<Session> session_a =
        fixture.make_flx_session(1, 0, db_a, sub_store_a, "/test", std::move(config_a));
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    Session::Config config_b;
    config_b.signed_user_token = g_user_1_path_test_token;
    config_b.user_id = "user_1";
    util::Optional<Session> session_b =
        fixture.make_flx_session(2, 0, db_b, sub_store_b, "/test", std::move(config_b));
    CHECK(session_b->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());
    CHECK(session_b->wait_for_download_complete_or_client_stopped());

    check_order_visibility(test_context, db_a, true, false);
    check_order_visibility(test_context, db_b, false, true);
    check_catalog_visible(test_context, db_a);
    check_catalog_visible(test_context, db_b);

    upsert_order_owner(seed_db, 1, "user_1");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());
    CHECK(session_b->wait_for_download_complete_or_client_stopped());
    check_order_visibility(test_context, db_a, false, false);
    check_order_visibility(test_context, db_b, true, true);

    upsert_order_owner(seed_db, 1, "user_0");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());
    CHECK(session_b->wait_for_download_complete_or_client_stopped());
    check_order_visibility(test_context, db_a, true, false);
    check_order_visibility(test_context, db_b, false, true);

    session_a.reset();

    upsert_order_owner(seed_db, 1, "user_1");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());
    CHECK(session_b->wait_for_download_complete_or_client_stopped());

    Session::Config reconnect_config_a;
    reconnect_config_a.signed_user_token = g_user_0_path_test_token;
    reconnect_config_a.user_id = "user_0";
    session_a = fixture.make_flx_session(1, 0, db_a, sub_store_a, "/test", std::move(reconnect_config_a));
    CHECK(session_a->wait_for_download_complete_or_client_stopped());
    check_order_visibility(test_context, db_a, false, false);
    check_order_visibility(test_context, db_b, true, true);
}

TEST(Sync_FLXBootstrapCacheDoesNotLeakBetweenUsers)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);
    TEST_CLIENT_DB(db_b);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    upsert_order_owner(seed_db, 2, "user_1");
    create_flx_isolation_schema(db_a, 0, nullptr, false);
    create_flx_isolation_schema(db_b, 0, nullptr, false);

    auto sub_store_a = SubscriptionStore::create(db_a);
    auto sub_store_b = SubscriptionStore::create(db_b);
    subscribe_to_flx_isolation_tables(db_a, sub_store_a);
    subscribe_to_flx_isolation_tables(db_b, sub_store_b);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    fixture_config.enable_download_bootstrap_cache = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(3, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    Session::Config config_a;
    config_a.signed_user_token = g_user_0_path_test_token;
    config_a.user_id = "user_0";
    util::Optional<Session> session_a =
        fixture.make_flx_session(1, 0, db_a, sub_store_a, "/test", std::move(config_a));
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    Session::Config config_b;
    config_b.signed_user_token = g_user_1_path_test_token;
    config_b.user_id = "user_1";
    util::Optional<Session> session_b =
        fixture.make_flx_session(2, 0, db_b, sub_store_b, "/test", std::move(config_b));
    CHECK(session_b->wait_for_upload_complete_or_client_stopped());
    CHECK(session_b->wait_for_download_complete_or_client_stopped());

    check_order_visibility(test_context, db_a, true, false);
    check_order_visibility(test_context, db_b, false, true);
    check_catalog_visible(test_context, db_a);
    check_catalog_visible(test_context, db_b);
}

TEST(Sync_FLXPBSAndFLXClientsCanUseSameServerOnDifferentTenants)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(pbs_writer_db);
    TEST_CLIENT_DB(pbs_reader_db);
    TEST_CLIENT_DB(flx_seed_db);
    TEST_CLIENT_DB(flx_user_db);

    create_flx_isolation_schema(pbs_writer_db, 42, "pbs_user", true);
    create_flx_isolation_schema(pbs_reader_db, 0, nullptr, false);
    create_flx_isolation_schema(flx_seed_db, 1, "user_0", true);
    create_flx_isolation_schema(flx_user_db, 0, nullptr, false);

    auto flx_sub_store = SubscriptionStore::create(flx_user_db);
    subscribe_to_flx_isolation_tables(flx_user_db, flx_sub_store);

    auto tenant_public_keys = std::make_shared<AccessControl::PublicKeyStore>();
    tenant_public_keys->keys["tenant-pbs"].push_back(PKey::load_public(test_util::get_test_resource_path() +
                                                                       "test_pubkey.pem"));
    tenant_public_keys->keys["tenant-flx"].push_back(PKey::load_public(test_util::get_test_resource_path() +
                                                                       "test_pubkey.pem"));

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    fixture_config.tenant_public_keys = tenant_public_keys;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(4, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    std::string pbs_real_path = fixture.map_virtual_to_real_path(0, "/tenant-pbs/pbs");
    std::string flx_real_path = fixture.map_virtual_to_real_path(0, "/tenant-flx/flx");
    CHECK_NOT_EQUAL(pbs_real_path, flx_real_path);
    CHECK(StringData{pbs_real_path}.ends_with("/tenant-pbs/pbs.barq"));
    CHECK(StringData{flx_real_path}.ends_with("/tenant-flx/flx.barq"));

    Session::Config pbs_writer_config;
    pbs_writer_config.user_id = "pbs_user";
    util::Optional<Session> pbs_writer_session =
        fixture.make_bound_session(0, pbs_writer_db, 0, "pbs", g_tenant_pbs_user_token,
                                   std::move(pbs_writer_config));
    CHECK(pbs_writer_session->wait_for_upload_complete_or_client_stopped());

    Session::Config flx_seed_config;
    flx_seed_config.user_id = "flx_seed";
    util::Optional<Session> flx_seed_session =
        fixture.make_bound_session(1, flx_seed_db, 0, "flx", g_tenant_flx_seed_token,
                                   std::move(flx_seed_config));
    CHECK(flx_seed_session->wait_for_upload_complete_or_client_stopped());

    Session::Config pbs_reader_config;
    pbs_reader_config.user_id = "pbs_user";
    util::Optional<Session> pbs_reader_session =
        fixture.make_bound_session(2, pbs_reader_db, 0, "pbs", g_tenant_pbs_user_token,
                                   std::move(pbs_reader_config));
    CHECK(pbs_reader_session->wait_for_download_complete_or_client_stopped());

    Session::Config flx_user_config;
    flx_user_config.signed_user_token = g_tenant_flx_user_token;
    flx_user_config.user_id = "user_0";
    util::Optional<Session> flx_user_session =
        fixture.make_flx_session(3, 0, flx_user_db, flx_sub_store, "flx", std::move(flx_user_config));
    CHECK(flx_user_session->wait_for_upload_complete_or_client_stopped());
    CHECK(flx_user_session->wait_for_download_complete_or_client_stopped());

    check_order_exists(test_context, pbs_reader_db, 42, true);
    check_order_visibility(test_context, flx_user_db, true, false);
    check_catalog_visible(test_context, flx_user_db);
    CHECK(util::File::exists(pbs_real_path));
    CHECK(util::File::exists(flx_real_path));

    upsert_order_owner(pbs_writer_db, 43, "pbs_user");
    CHECK(pbs_writer_session->wait_for_upload_complete_or_client_stopped());
    CHECK(pbs_reader_session->wait_for_download_complete_or_client_stopped());
    CHECK(flx_user_session->wait_for_download_complete_or_client_stopped());
    check_order_exists(test_context, pbs_reader_db, 43, true);
    check_order_visibility(test_context, flx_user_db, true, false);

    upsert_order_owner(flx_seed_db, 2, "user_0");
    CHECK(flx_seed_session->wait_for_upload_complete_or_client_stopped());
    CHECK(flx_user_session->wait_for_download_complete_or_client_stopped());
    CHECK(pbs_reader_session->wait_for_download_complete_or_client_stopped());
    check_order_visibility(test_context, flx_user_db, true, true);
    check_order_exists(test_context, pbs_reader_db, 2, false);
}

TEST(Sync_FLXUsesTokenPathNotBindIdentifier)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    create_flx_isolation_schema(db, 0, nullptr, false);

    auto sub_store = SubscriptionStore::create(db);
    subscribe_to_flx_isolation_tables(db, sub_store);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    std::string token_real_path = fixture.map_virtual_to_real_path(0, "/test");
    std::string ignored_real_path = fixture.map_virtual_to_real_path(0, "/ignored-bind-path");

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());
    CHECK(util::File::exists(token_real_path));
    CHECK_NOT(util::File::exists(ignored_real_path));

    Session::Config config;
    config.signed_user_token = g_user_0_path_test_token;
    config.user_id = "user_0";
    util::Optional<Session> session =
        fixture.make_flx_session(1, 0, db, sub_store, "/ignored-bind-path", std::move(config));
    CHECK(session->wait_for_upload_complete_or_client_stopped());
    CHECK(session->wait_for_download_complete_or_client_stopped());

    check_order_visibility(test_context, db, true, false);
    check_catalog_visible(test_context, db);
    CHECK(util::File::exists(token_real_path));
    CHECK_NOT(util::File::exists(ignored_real_path));
}

#if defined(BARQ_SYNC_SERVER_BINARY) && !defined(_WIN32) && !BARQ_ANDROID && !BARQ_IOS
TEST(Sync_FLXServerBinaryRejectsQuerySyncWhenDisabled)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db);

    create_flx_isolation_schema(db, 0, nullptr, false);
    auto sub_store = SubscriptionStore::create(db);
    subscribe_to_flx_isolation_tables(db, sub_store);

    std::string server_root = util::File::resolve("server-root", dir);
    util::make_dir_recursive(server_root);

    ExternalServerProcess server({
        BARQ_SYNC_SERVER_BINARY,
        "--root-dir",
        server_root,
        "--jwt-public-key",
        test_server_key_path(),
        "--host",
        "127.0.0.1",
        "--port",
        "0",
        "--log-level",
        "warn",
    });
    Session::port_type port = parse_external_cli_server_port(server.wait_for_route());

    ExternalSyncClient client(std::make_shared<util::PrefixLogger>("CLI disabled FLX: ", test_context.logger));
    std::atomic<bool> did_reject{false};

    Session::Config config = make_external_cli_session_config(port, g_user_0_path_test_token, "user_0", test_context);
    config.connection_state_change_listener = [&](ConnectionState state, std::optional<SessionErrorInfo> error_info) {
        if (state == ConnectionState::disconnected && error_info) {
            did_reject.store(true);
            client.get().shutdown();
        }
    };

    util::Optional<Session> session = Session{
        client.get(),
        db,
        sub_store,
        MigrationStore::create(db),
        std::move(config),
    };
    CHECK_NOT(session->wait_for_download_complete_or_client_stopped());
    CHECK(did_reject.load());
}

TEST(Sync_FLXServerBinaryCLIEnforcesRulesEndToEnd)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);
    TEST_CLIENT_DB(db_b);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    upsert_order_owner(seed_db, 2, "user_1");
    create_flx_isolation_schema(db_a, 0, nullptr, false);
    create_flx_isolation_schema(db_b, 0, nullptr, false);

    auto sub_store_a = SubscriptionStore::create(db_a);
    auto sub_store_b = SubscriptionStore::create(db_b);
    subscribe_to_flx_isolation_tables(db_a, sub_store_a);
    subscribe_to_flx_isolation_tables(db_b, sub_store_b);

    std::string server_root = util::File::resolve("server-root", dir);
    util::make_dir_recursive(server_root);

    ExternalServerProcess server({
        BARQ_SYNC_SERVER_BINARY,
        "--root-dir",
        server_root,
        "--jwt-public-key",
        test_server_key_path(),
        "--host",
        "127.0.0.1",
        "--port",
        "0",
        "--enable-flx",
        "--flx-owner-rule",
        "Order:owner_id",
        "--flx-public-readonly-rule",
        "Catalog",
        "--log-level",
        "warn",
    });
    Session::port_type port = parse_external_cli_server_port(server.wait_for_route());

    ExternalSyncClient seed_client(std::make_shared<util::PrefixLogger>("CLI seed: ", test_context.logger));
    ExternalSyncClient client_a(std::make_shared<util::PrefixLogger>("CLI user A: ", test_context.logger));
    ExternalSyncClient client_b(std::make_shared<util::PrefixLogger>("CLI user B: ", test_context.logger));

    util::Optional<Session> seed_session = Session{
        seed_client.get(),
        seed_db,
        nullptr,
        nullptr,
        make_external_cli_session_config(port, g_signed_test_user_token, "test", test_context),
    };
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());
    CHECK(util::File::exists(util::File::resolve("test.barq", server_root)));

    util::Optional<Session> session_a = Session{
        client_a.get(),
        db_a,
        sub_store_a,
        MigrationStore::create(db_a),
        make_external_cli_session_config(port, g_user_0_path_test_token, "user_0", test_context),
    };
    util::Optional<Session> session_b = Session{
        client_b.get(),
        db_b,
        sub_store_b,
        MigrationStore::create(db_b),
        make_external_cli_session_config(port, g_user_1_path_test_token, "user_1", test_context),
    };

    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_b->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());
    CHECK(session_b->wait_for_download_complete_or_client_stopped());

    check_order_visibility(test_context, db_a, true, false);
    check_order_visibility(test_context, db_b, false, true);
    check_catalog_visible(test_context, db_a);
    check_catalog_visible(test_context, db_b);

    upsert_order_owner(seed_db, 1, "user_1");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());
    CHECK(session_b->wait_for_download_complete_or_client_stopped());

    check_order_visibility(test_context, db_a, false, false);
    check_order_visibility(test_context, db_b, true, true);
}

TEST(Sync_FLXServerBinaryLiveRuleAPIValidatesPersistsAndIsolatesScopes)
{
    TEST_DIR(dir);
    std::string server_root = util::File::resolve("server-root", dir);
    util::make_dir_recursive(server_root);

    const std::string scope = R"({"tenant":"tenant-a","database":"main"})";
    const std::string schema =
        R"({"scope":)" + scope +
        R"(,"version":1,"manifest":{"objects":[{"name":"Order","primary_key":{"name":"id","type":"string"},"properties":[{"name":"id","type":"string"},{"name":"owner_id","type":"string"}]}]}})";
    const std::string owner_rules =
        R"({"scope":)" + scope +
        R"(,"expected_revision":0,"target_revision":1,"rules":[{"object_type":"Order","read":"owner_id == $user.id","write":"owner_id == $user.id"}]})";

    {
        ExternalServerProcess server({
            BARQ_SYNC_SERVER_BINARY,
            "--root-dir",
            server_root,
            "--jwt-public-key",
            test_server_key_path(),
            "--host",
            "127.0.0.1",
            "--port",
            "0",
            "--enable-flx",
            "--internal-api-secret",
            "test-internal-secret",
            "--log-level",
            "warn",
        });
        Session::port_type port = parse_external_cli_server_port(server.wait_for_route());

        HTTPResponse schema_response =
            call_external_internal_api(port, "/internal/v1/schema/apply", schema, test_context);
        CHECK_EQUAL(schema_response.status, HTTPStatus::Ok);

        std::string invalid =
            R"({"scope":)" + scope +
            R"json(,"expected_revision":0,"rules":[{"object_type":"Order","read":"TRUEPREDICATE SORT(id ASC)","write":"FALSEPREDICATE"}]})json";
        HTTPResponse invalid_response =
            call_external_internal_api(port, "/internal/v1/flx/rules/plan", invalid, test_context);
        CHECK_EQUAL(invalid_response.status, HTTPStatus::BadRequest);

        HTTPResponse apply_response =
            call_external_internal_api(port, "/internal/v1/flx/rules/apply", owner_rules, test_context);
        CHECK_EQUAL(apply_response.status, HTTPStatus::Ok);
        CHECK(bool(apply_response.body));
        if (apply_response.body)
            CHECK(apply_response.body->find(R"("revision":1)") != std::string::npos);

        HTTPResponse retry_response =
            call_external_internal_api(port, "/internal/v1/flx/rules/apply", owner_rules, test_context);
        CHECK_EQUAL(retry_response.status, HTTPStatus::Ok);

        std::string stale =
            R"({"scope":)" + scope +
            R"(,"expected_revision":0,"target_revision":1,"rules":[{"object_type":"Order","read":"TRUEPREDICATE","write":"TRUEPREDICATE"}]})";
        HTTPResponse stale_response =
            call_external_internal_api(port, "/internal/v1/flx/rules/apply", stale, test_context);
        CHECK_EQUAL(stale_response.status, HTTPStatus::Conflict);

        std::string other_scope = R"({"scope":{"tenant":"tenant-b","database":"main"}})";
        HTTPResponse other_response =
            call_external_internal_api(port, "/internal/v1/flx/rules/read", other_scope, test_context);
        CHECK_EQUAL(other_response.status, HTTPStatus::Ok);
        CHECK(bool(other_response.body));
        if (other_response.body)
            CHECK(other_response.body->find(R"("revision":0)") != std::string::npos);
    }

    {
        ExternalServerProcess server({
            BARQ_SYNC_SERVER_BINARY,
            "--root-dir",
            server_root,
            "--jwt-public-key",
            test_server_key_path(),
            "--host",
            "127.0.0.1",
            "--port",
            "0",
            "--enable-flx",
            "--internal-api-secret",
            "test-internal-secret",
            "--log-level",
            "warn",
        });
        Session::port_type port = parse_external_cli_server_port(server.wait_for_route());
        HTTPResponse read_response =
            call_external_internal_api(port, "/internal/v1/flx/rules/read", R"({"scope":)" + scope + "}",
                                       test_context);
        CHECK_EQUAL(read_response.status, HTTPStatus::Ok);
        CHECK(bool(read_response.body));
        if (read_response.body) {
            CHECK(read_response.body->find(R"("revision":1)") != std::string::npos);
            CHECK(read_response.body->find(R"("source":"database")") != std::string::npos);
        }
    }
}

TEST(Sync_FLXServerBinaryLiveRulesRefilterConnectedDevice)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(user_db);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    upsert_order_owner(seed_db, 2, "user_1");
    create_flx_isolation_schema(user_db, 0, nullptr, false);
    auto subscriptions = SubscriptionStore::create(user_db);
    subscribe_to_flx_isolation_tables(user_db, subscriptions);

    std::string server_root = util::File::resolve("server-root", dir);
    util::make_dir_recursive(server_root);
    ExternalServerProcess server({
        BARQ_SYNC_SERVER_BINARY,
        "--root-dir",
        server_root,
        "--allow-unsigned-tokens",
        "--host",
        "127.0.0.1",
        "--port",
        "0",
        "--enable-flx",
        "--internal-api-secret",
        "test-internal-secret",
        "--log-level",
        "warn",
    });
    Session::port_type port = parse_external_cli_server_port(server.wait_for_route());

    ExternalSyncClient seed_client(std::make_shared<util::PrefixLogger>("live rules seed: ", test_context.logger));
    Session::Config seed_config =
        make_external_cli_session_config(port, g_live_rules_seed_token, "seed", test_context);
    seed_config.barq_identifier = "tenant-a/main";
    util::Optional<Session> seed_session = Session{seed_client.get(), seed_db, nullptr, nullptr, std::move(seed_config)};
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    ExternalSyncClient user_client(std::make_shared<util::PrefixLogger>("live rules user: ", test_context.logger));
    util::Optional<Session> user_session =
        Session{user_client.get(), user_db, subscriptions, MigrationStore::create(user_db),
                make_external_cli_session_config(port, g_live_rules_user_token, "user_0", test_context)};
    CHECK(user_session->wait_for_download_complete_or_client_stopped());
    check_order_visibility(test_context, user_db, false, false);

    const std::string scope = R"({"tenant":"tenant-a","database":"main"})";
    auto apply = [&](uint64_t expected, const std::string& read, const std::string& write) {
        std::string body = R"({"scope":)" + scope + R"(,"expected_revision":)" + util::to_string(expected) +
                           R"(,"target_revision":)" + util::to_string(expected + 1) +
                           R"(,"rules":[{"object_type":"Order","read":")" + read +
                           R"(","write":")" + write + R"("}]})";
        HTTPResponse response =
            call_external_internal_api(port, "/internal/v1/flx/rules/apply", body, test_context);
        CHECK_EQUAL(response.status, HTTPStatus::Ok);
    };

    apply(0, "owner_id == $user.id", "owner_id == $user.id");
    CHECK(user_session->wait_for_download_complete_or_client_stopped());
    check_order_visibility(test_context, user_db, true, false);

    // Queue a device write while the rule revision changes. The database writer
    // serializes both operations, and the final view follows the new revision.
    upsert_order_owner(seed_db, 1, "user_1");
    apply(1, "TRUEPREDICATE", "TRUEPREDICATE");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());
    CHECK(user_session->wait_for_download_complete_or_client_stopped());
    check_order_visibility(test_context, user_db, true, true);

    apply(2, "owner_id == $user.id", "owner_id == $user.id");
    CHECK(user_session->wait_for_download_complete_or_client_stopped());
    check_order_visibility(test_context, user_db, false, false);
    ReadTransaction read{user_db};
    CHECK_NOT(read.get_table("flx_rule_state"));
}
#endif

TEST(Sync_FLXVisibleStateSurvivesServerRestart)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    create_flx_isolation_schema(db_a, 0, nullptr, false);

    auto sub_store_a = SubscriptionStore::create(db_a);
    subscribe_to_flx_isolation_tables(db_a, sub_store_a);

    auto make_fixture_config = [] {
        MultiClientServerFixture::Config fixture_config;
        fixture_config.enable_flx_sync = true;
        using FLXRule = Server::Config::FLXRule;
        fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
        fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});
        return fixture_config;
    };

    {
        MultiClientServerFixture fixture(2, 1, dir, test_context, make_fixture_config());
        fixture.start();

        util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
        CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

        Session::Config config_a;
        config_a.signed_user_token = g_user_0_path_test_token;
        config_a.user_id = "user_0";
        util::Optional<Session> session_a =
            fixture.make_flx_session(1, 0, db_a, sub_store_a, "/test", std::move(config_a));
        CHECK(session_a->wait_for_upload_complete_or_client_stopped());
        CHECK(session_a->wait_for_download_complete_or_client_stopped());
        check_order_visibility(test_context, db_a, true, false);

        session_a.reset();
        upsert_order_owner(seed_db, 1, "user_1");
        CHECK(seed_session->wait_for_upload_complete_or_client_stopped());
        seed_session.reset();
    }

    {
        MultiClientServerFixture fixture(1, 1, dir, test_context, make_fixture_config());
        fixture.start();

        Session::Config reconnect_config_a;
        reconnect_config_a.signed_user_token = g_user_0_path_test_token;
        reconnect_config_a.user_id = "user_0";
        util::Optional<Session> session_a =
            fixture.make_flx_session(0, 0, db_a, sub_store_a, "/test", std::move(reconnect_config_a));
        CHECK(session_a->wait_for_download_complete_or_client_stopped());
        check_order_visibility(test_context, db_a, false, false);
    }
}

TEST(Sync_FLXVisibleStateGarbageCollectsExpiredClients)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    create_flx_isolation_schema(db_a, 0, nullptr, false);

    auto sub_store_a = SubscriptionStore::create(db_a);
    subscribe_to_flx_isolation_tables(db_a, sub_store_a);

    auto make_fixture_config = [] {
        MultiClientServerFixture::Config fixture_config;
        fixture_config.enable_flx_sync = true;
        using FLXRule = Server::Config::FLXRule;
        fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
        fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});
        return fixture_config;
    };

    std::string server_real_path;

    // A live FLX client connects and persists its per-client visible-state row.
    {
        MultiClientServerFixture fixture(2, 1, dir, test_context, make_fixture_config());
        fixture.start();
        server_real_path = fixture.map_virtual_to_real_path(0, "/test");

        util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
        CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

        Session::Config config_a;
        config_a.signed_user_token = g_user_0_path_test_token;
        config_a.user_id = "user_0";
        util::Optional<Session> session_a =
            fixture.make_flx_session(1, 0, db_a, sub_store_a, "/test", std::move(config_a));
        CHECK(session_a->wait_for_upload_complete_or_client_stopped());
        CHECK(session_a->wait_for_download_complete_or_client_stopped());
        check_order_visibility(test_context, db_a, true, false);

        session_a.reset();
        seed_session.reset();
    }

    // Inject an orphan visible-state row for a client file identifier that was
    // never allocated. This stands in for a client whose file has since been
    // expired (its live entry would report last_seen == 0 just the same).
    constexpr int64_t orphan_ident = 987654;
    {
        TestServerHistoryContext context;
        _impl::ServerHistory history{context};
        DBRef server_db = DB::create(history, server_real_path);
        write_transaction(server_db, [&](WriteTransaction& wt) {
            auto table = wt.get_group().get_table("flx_visible_state");
            CHECK(table);
            auto row = table->create_object_with_primary_key(orphan_ident);
            row.set("visible_json", "[]");
        });
        auto rt = server_db->start_read();
        auto table = rt->get_table("flx_visible_state");
        CHECK(table);
        CHECK_EQUAL(table->size(), 2); // live client row + injected orphan
        CHECK(table->get_object_with_primary_key(orphan_ident));
    }

    // The first Flexible Sync bind after a restart garbage-collects visible-
    // state rows for clients that are gone. Reconnecting the live client both
    // triggers the sweep and confirms the live client keeps working.
    {
        MultiClientServerFixture fixture(1, 1, dir, test_context, make_fixture_config());
        fixture.start();
        Session::Config config_a;
        config_a.signed_user_token = g_user_0_path_test_token;
        config_a.user_id = "user_0";
        util::Optional<Session> session_a =
            fixture.make_flx_session(0, 0, db_a, sub_store_a, "/test", std::move(config_a));
        CHECK(session_a->wait_for_download_complete_or_client_stopped());
        check_order_visibility(test_context, db_a, true, false);
        session_a.reset();
    }

    // The orphan row is reclaimed; the live (non-expired) client's row is kept.
    {
        TestServerHistoryContext context;
        _impl::ServerHistory history{context};
        DBRef server_db = DB::create(history, server_real_path);
        auto rt = server_db->start_read();
        auto table = rt->get_table("flx_visible_state");
        CHECK(table);
        CHECK_NOT(table->get_object_with_primary_key(orphan_ident));
        CHECK_EQUAL(table->size(), 1);
    }
}

TEST(Sync_FLXManyObjectsAndUsersIsolate)
{
    // A larger-than-toy check: thousands of owner-scoped rows on one shared file,
    // several concurrent clients, and an admin aggregating across all of them.
    // Proves the rule/subscription filtering holds well beyond the tiny datasets
    // the other FLX tests use. (True millions-of-rows / thousands-of-users load
    // testing needs a dedicated release-build harness; see doc/flexible_sync.md.)
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_0);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_admin);

    constexpr int64_t per_user = 500;
    constexpr std::size_t catalog_count = 11; // id 10 (from schema) + ids 11..20

    create_flx_isolation_schema(seed_db, 0, nullptr, true); // schema + catalog id 10
    add_flx_catalog_items(seed_db, 11, 20);                 // + 10 more shared products
    add_flx_orders(seed_db, 1, per_user, "user_0");
    add_flx_orders(seed_db, per_user + 1, 2 * per_user, "user_1");

    create_flx_isolation_schema(db_0, 0, nullptr, false);
    create_flx_isolation_schema(db_1, 0, nullptr, false);
    create_flx_isolation_schema(db_admin, 0, nullptr, false);

    auto sub_0 = SubscriptionStore::create(db_0);
    auto sub_1 = SubscriptionStore::create(db_1);
    auto sub_admin = SubscriptionStore::create(db_admin);
    subscribe_to_flx_isolation_tables(db_0, sub_0);
    subscribe_to_flx_isolation_tables(db_1, sub_1);
    subscribe_to_flx_isolation_tables(db_admin, sub_admin);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(4, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    auto check_all_owned_by = [&](DBRef db, const char* owner, std::size_t expected) {
        auto rt = db->start_read();
        auto orders = rt->get_table("class_Order");
        CHECK_EQUAL(orders->size(), expected);
        ColKey owner_col = orders->get_column_key("owner_id");
        std::size_t foreign = 0;
        for (auto& obj : *orders) {
            if (obj.get<StringData>(owner_col) != StringData(owner))
                ++foreign;
        }
        CHECK_EQUAL(foreign, 0); // not one row belonging to another user leaked in
        CHECK_EQUAL(rt->get_table("class_Catalog")->size(), catalog_count);
    };

    // Each user bootstraps and must see exactly its own `per_user` orders plus
    // the shared catalog -- nothing belonging to the other user.
    Session::Config config_0;
    config_0.signed_user_token = g_user_0_path_test_token;
    config_0.user_id = "user_0";
    util::Optional<Session> session_0 = fixture.make_flx_session(1, 0, db_0, sub_0, "/test", std::move(config_0));
    CHECK(session_0->wait_for_upload_complete_or_client_stopped());
    CHECK(session_0->wait_for_download_complete_or_client_stopped());
    check_all_owned_by(db_0, "user_0", std::size_t(per_user));

    Session::Config config_1;
    config_1.signed_user_token = g_user_1_path_test_token;
    config_1.user_id = "user_1";
    util::Optional<Session> session_1 = fixture.make_flx_session(2, 0, db_1, sub_1, "/test", std::move(config_1));
    CHECK(session_1->wait_for_upload_complete_or_client_stopped());
    CHECK(session_1->wait_for_download_complete_or_client_stopped());
    check_all_owned_by(db_1, "user_1", std::size_t(per_user));

    // The admin aggregates across every user.
    Session::Config config_admin;
    config_admin.signed_user_token = g_admin_path_test_token;
    config_admin.user_id = "admin";
    util::Optional<Session> session_admin =
        fixture.make_flx_session(3, 0, db_admin, sub_admin, "/test", std::move(config_admin));
    CHECK(session_admin->wait_for_upload_complete_or_client_stopped());
    CHECK(session_admin->wait_for_download_complete_or_client_stopped());

    auto rt_admin = db_admin->start_read();
    CHECK_EQUAL(rt_admin->get_table("class_Order")->size(), std::size_t(2 * per_user));
    CHECK_EQUAL(rt_admin->get_table("class_Catalog")->size(), catalog_count);
}

TEST(Sync_FLXNonAdminCannotDropSchemaColumn)
{
    // A non-admin Flexible Sync client must not be able to destroy the shared
    // schema: dropping a table or column can't be undone by the compensating-
    // write pass, so the server rejects the upload before integrating it and the
    // shared schema is left intact.
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    create_flx_isolation_schema(db_a, 0, nullptr, false);

    auto sub_a = SubscriptionStore::create(db_a);
    subscribe_to_flx_isolation_tables(db_a, sub_a);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));

    std::mutex reject_mutex;
    bool did_reject = false;
    fixture.set_client_side_error_handler(1, [&](Status status, bool) {
        std::lock_guard<std::mutex> lock(reject_mutex);
        CHECK_NOT(status.is_ok());
        did_reject = true;
        fixture.get_client(1).shutdown();
    });

    fixture.start();
    std::string server_real_path = fixture.map_virtual_to_real_path(0, "/test");

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    Session::Config config_a;
    config_a.signed_user_token = g_user_0_path_test_token;
    config_a.user_id = "user_0";
    util::Optional<Session> session_a = fixture.make_flx_session(1, 0, db_a, sub_a, "/test", std::move(config_a));
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());
    check_order_visibility(test_context, db_a, true, false);

    // Drop a synced column locally, then try to upload the destructive change.
    write_transaction(db_a, [&](WriteTransaction& wt) {
        auto orders = wt.get_group().get_table("class_Order");
        orders->remove_column(orders->get_column_key("item"));
    });
    CHECK_NOT(session_a->wait_for_upload_complete_or_client_stopped());
    {
        std::lock_guard<std::mutex> lock(reject_mutex);
        CHECK(did_reject);
    }

    // The shared server schema is intact: the "item" column was NOT dropped.
    TestServerHistoryContext context;
    _impl::ServerHistory history{context};
    DBRef server_db = DB::create(history, server_real_path);
    auto rt = server_db->start_read();
    auto orders = rt->get_table("class_Order");
    CHECK(orders);
    if (orders) {
        CHECK(orders->get_column_key("item"));
    }
}

TEST(Sync_FLXOwnerRuleNonStringFieldReturnsQueryErrorAndKeepsSessionAlive)
{
    // An owner rule whose column is not a string is a misconfiguration. The
    // server must reject the subscription with a clear query error and keep the
    // session alive -- it must never call Mixed::get<StringData>() on the
    // non-string column (which would crash the network thread).
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db);

    auto make_people_with_int_owner = [](DBRef d, bool add_row) {
        write_transaction(d, [&](WriteTransaction& wt) {
            auto people = wt.get_group().add_table_with_primary_key("class_Person", type_Int, "id");
            people->add_column(type_Int, "age");
            people->add_column(type_Int, "owner_id"); // non-string on purpose
            if (add_row) {
                auto p = people->create_object_with_primary_key(1);
                p.set("age", int64_t(30));
                p.set("owner_id", int64_t(7));
            }
        });
    };
    make_people_with_int_owner(seed_db, true);
    make_people_with_int_owner(db, false);

    auto sub_store = SubscriptionStore::create(db);
    auto bad_sub_set = subscribe_to_adult_people(db, sub_store);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Person", FLXRule::Mode::Owner, "owner_id"});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    Session::Config config;
    config.signed_user_token = g_user_0_path_test_token;
    config.user_id = "user_0";
    util::Optional<Session> session = fixture.make_flx_session(1, 0, db, sub_store, "/test", std::move(config));

    CHECK(wait_for_subscription_state(bad_sub_set, SubscriptionSet::State::Error));
    CHECK(bad_sub_set.error_str().contains("string owner field"));
    CHECK(bad_sub_set.error_str().contains("Person"));

    // The session survives the bad subscription: an empty subscription completes.
    auto mut = sub_store->get_latest().make_mutable_copy();
    mut.clear();
    auto empty_sub_set = mut.commit();
    CHECK(session->wait_for_upload_complete_or_client_stopped());
    CHECK(session->wait_for_download_complete_or_client_stopped());
    CHECK(wait_for_subscription_state(empty_sub_set, SubscriptionSet::State::Complete));
}

TEST(Sync_FLXOwnerColumnRetypedToNonStringDoesNotCrash)
{
    // The read-path owner guard's real job. The owner column passes validation as
    // a string when the client subscribes, but is later re-typed to a non-string
    // by a partition-sync writer (which is not subject to FLX schema rules). The
    // next incremental download must not call Mixed::get<StringData>() on the
    // now-integer column -- with the guard the row simply becomes invisible; on
    // the network thread that would otherwise crash the whole server.
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    create_flx_isolation_schema(db_a, 0, nullptr, false);

    auto sub_a = SubscriptionStore::create(db_a);
    subscribe_to_flx_isolation_tables(db_a, sub_a);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    Session::Config config_a;
    config_a.signed_user_token = g_user_0_path_test_token;
    config_a.user_id = "user_0";
    util::Optional<Session> session_a = fixture.make_flx_session(1, 0, db_a, sub_a, "/test", std::move(config_a));
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());
    check_order_visibility(test_context, db_a, true, false); // visible while owner_id is a string

    // A partition-sync writer re-types owner_id (string -> int) and touches the row
    // so the change reaches db_a's subscription as an incremental download.
    write_transaction(seed_db, [&](WriteTransaction& wt) {
        auto orders = wt.get_group().get_table("class_Order");
        orders->remove_column(orders->get_column_key("owner_id"));
        orders->add_column(type_Int, "owner_id");
        auto order = orders->get_object_with_primary_key(1);
        order.set("item", "retyped");
    });
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    // The server must survive building db_a's next download. Non-string owner ->
    // default deny, so the order leaves the view and the session stays alive.
    CHECK(session_a->wait_for_download_complete_or_client_stopped());
    check_order_count(test_context, db_a, 0);
}

TEST(Sync_FLXDisabledRejectsQuerySyncClients)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db);

    create_flx_isolation_schema(db, 0, nullptr, false);
    auto sub_store = SubscriptionStore::create(db);
    subscribe_to_flx_isolation_tables(db, sub_store);

    MultiClientServerFixture fixture(1, 1, dir, test_context);
    bool did_reject = false;
    fixture.set_client_side_error_handler(0, [&](Status status, bool) {
        CHECK_NOT(status.is_ok());
        did_reject = true;
        fixture.get_client(0).shutdown();
    });
    fixture.start();

    Session::Config config;
    config.signed_user_token = g_user_0_path_test_token;
    config.user_id = "user_0";
    util::Optional<Session> session =
        fixture.make_flx_session(0, 0, db, sub_store, "/test", std::move(config));
    CHECK_NOT(session->wait_for_download_complete_or_client_stopped());
    CHECK(did_reject);
}

TEST(Sync_FLXMissingTableSubscriptionReturnsEmpty)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(db);

    create_flx_person_schema(db);
    auto sub_store = SubscriptionStore::create(db);
    auto sub_set = subscribe_to_adult_people(db, sub_store);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Person", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(1, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    Session::Config config;
    config.signed_user_token = g_user_0_path_test_token;
    config.user_id = "user_0";
    util::Optional<Session> session = fixture.make_flx_session(0, 0, db, sub_store, "/test", std::move(config));
    CHECK(session->wait_for_upload_complete_or_client_stopped());
    CHECK(session->wait_for_download_complete_or_client_stopped());

    sub_set.refresh();
    CHECK_EQUAL(sub_set.state(), SubscriptionSet::State::Complete);
    CHECK(sub_set.error_str().is_null());
    check_person_count(test_context, db, 0);
}

TEST(Sync_FLXInvalidRQLReturnsQueryErrorAndKeepsSessionAlive)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db);

    create_flx_person_schema(seed_db);
    upsert_flx_person(seed_db, 1, 30);
    create_flx_person_schema(db);

    auto sub_store = SubscriptionStore::create(db);
    auto bad_sub_set = subscribe_to_adult_people(db, sub_store);
    replace_subscription_query(test_context, db, bad_sub_set.version(), "missing_property > 18");

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Person", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    Session::Config config;
    config.signed_user_token = g_user_0_path_test_token;
    config.user_id = "user_0";
    util::Optional<Session> session = fixture.make_flx_session(1, 0, db, sub_store, "/test", std::move(config));

    CHECK(wait_for_subscription_state(bad_sub_set, SubscriptionSet::State::Error));
    CHECK(bad_sub_set.error_str().contains("Bad subscription query"));
    CHECK(bad_sub_set.error_str().contains("Person"));

    auto good_sub_set = subscribe_to_adult_people(db, sub_store);
    CHECK(session->wait_for_upload_complete_or_client_stopped());
    CHECK(session->wait_for_download_complete_or_client_stopped());
    CHECK(wait_for_subscription_state(good_sub_set, SubscriptionSet::State::Complete));
    CHECK(good_sub_set.error_str().is_null());
    check_person_count(test_context, db, 1);
}

TEST(Sync_FLXOwnerRuleMissingOwnerFieldReturnsQueryErrorAndKeepsSessionAlive)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db);

    create_flx_person_schema(seed_db);
    upsert_flx_person(seed_db, 1, 30);
    create_flx_person_schema(db);

    auto sub_store = SubscriptionStore::create(db);
    auto bad_sub_set = subscribe_to_adult_people(db, sub_store);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Person", FLXRule::Mode::Owner, "owner_id"});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    Session::Config config;
    config.signed_user_token = g_user_0_path_test_token;
    config.user_id = "user_0";
    util::Optional<Session> session = fixture.make_flx_session(1, 0, db, sub_store, "/test", std::move(config));

    CHECK(wait_for_subscription_state(bad_sub_set, SubscriptionSet::State::Error));
    CHECK(bad_sub_set.error_str().contains("missing owner field"));
    CHECK(bad_sub_set.error_str().contains("Person"));

    auto mut = sub_store->get_latest().make_mutable_copy();
    mut.clear();
    auto empty_sub_set = mut.commit();
    CHECK(session->wait_for_upload_complete_or_client_stopped());
    CHECK(session->wait_for_download_complete_or_client_stopped());
    CHECK(wait_for_subscription_state(empty_sub_set, SubscriptionSet::State::Complete));
}

TEST(Sync_FLXDefaultDenyReadReturnsEmptyForExistingTable)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db);

    create_flx_isolation_schema(seed_db, 0, nullptr, false);
    add_flx_private_schema(seed_db);
    upsert_flx_private_object(seed_db, 1, "server-private");
    create_flx_isolation_schema(db, 0, nullptr, false);
    add_flx_private_schema(db);

    auto sub_store = SubscriptionStore::create(db);
    auto sub_set = subscribe_to_flx_private_table(db, sub_store);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    Session::Config config;
    config.signed_user_token = g_user_0_path_test_token;
    config.user_id = "user_0";
    util::Optional<Session> session = fixture.make_flx_session(1, 0, db, sub_store, "/test", std::move(config));
    CHECK(session->wait_for_upload_complete_or_client_stopped());
    CHECK(session->wait_for_download_complete_or_client_stopped());

    CHECK(wait_for_subscription_state(sub_set, SubscriptionSet::State::Complete));
    sub_set.refresh();
    CHECK(sub_set.error_str().is_null());
    check_private_count(test_context, db, 0);
}

TEST(Sync_FLXBootstrapCanSpanMultipleBatches)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db);

    constexpr int64_t order_count = 25;
    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    add_flx_orders(seed_db, 2, order_count, "user_0");
    create_flx_isolation_schema(db, 0, nullptr, false);

    auto sub_store = SubscriptionStore::create(db);
    subscribe_to_flx_isolation_tables(db, sub_store);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    fixture_config.max_download_size = 96;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    std::mutex batch_mutex;
    int more_to_come_count = 0;
    int last_in_batch_count = 0;

    Session::Config config;
    config.signed_user_token = g_user_0_path_test_token;
    config.user_id = "user_0";
    config.on_sync_client_event_hook = [&](const SyncClientHookData& data) {
        if (data.event == SyncClientHookEvent::DownloadMessageReceived && data.query_version == 1) {
            std::lock_guard<std::mutex> lock(batch_mutex);
            if (data.batch_state == sync::DownloadBatchState::MoreToCome) {
                ++more_to_come_count;
            }
            else if (data.batch_state == sync::DownloadBatchState::LastInBatch) {
                ++last_in_batch_count;
            }
        }
        return SyncClientHookAction::NoAction;
    };

    util::Optional<Session> session = fixture.make_flx_session(1, 0, db, sub_store, "/test", std::move(config));
    CHECK(session->wait_for_upload_complete_or_client_stopped());
    CHECK(session->wait_for_download_complete_or_client_stopped());

    check_order_count(test_context, db, order_count);
    check_catalog_visible(test_context, db);

    {
        std::lock_guard<std::mutex> lock(batch_mutex);
        CHECK_GREATER(more_to_come_count, 0);
        CHECK_EQUAL(last_in_batch_count, 1);
    }
}

TEST(Sync_FLXBootstrapUsesSingleSnapshotAcrossBatches)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db);

    constexpr int64_t order_count = 40;
    constexpr int64_t target_order_id = order_count;
    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    add_flx_orders(seed_db, 2, order_count, "user_0");
    create_flx_isolation_schema(db, 0, nullptr, false);

    auto sub_store = SubscriptionStore::create(db);
    subscribe_to_flx_isolation_tables(db, sub_store);

    std::string server_real_path;
    std::atomic<bool> mutated_server_snapshot_target{false};
    std::atomic<bool> checked_bootstrap_snapshot{false};

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    fixture_config.max_download_size = 1;
    fixture_config.server_flx_bootstrap_batch_callback =
        [&](std::string_view virt_path, file_ident_type, sync::DownloadBatchState batch_state, std::size_t) {
            if (virt_path != "/test" || batch_state != sync::DownloadBatchState::MoreToCome) {
                return;
            }
            bool expected = false;
            if (!mutated_server_snapshot_target.compare_exchange_strong(expected, true)) {
                return;
            }
            set_server_order_owner_directly(server_real_path, target_order_id, "user_1");
        };

    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();
    server_real_path = fixture.map_virtual_to_real_path(0, "/test");

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    Session::Config config;
    config.signed_user_token = g_user_0_path_test_token;
    config.user_id = "user_0";
    config.on_sync_client_event_hook = [&](const SyncClientHookData& data) {
        if (data.event == SyncClientHookEvent::BootstrapProcessed && data.query_version == 1) {
            check_order_count(test_context, db, order_count);
            check_order_owner(test_context, db, target_order_id, "user_0");
            checked_bootstrap_snapshot.store(true);
        }
        return SyncClientHookAction::NoAction;
    };
    util::Optional<Session> session = fixture.make_flx_session(1, 0, db, sub_store, "/test", std::move(config));
    CHECK(session->wait_for_upload_complete_or_client_stopped());
    CHECK(session->wait_for_download_complete_or_client_stopped());

    CHECK(mutated_server_snapshot_target.load());
    CHECK(checked_bootstrap_snapshot.load());
    check_order_count(test_context, db, order_count - 1);
    check_order_exists(test_context, db, target_order_id, false);
}

TEST(Sync_FLXIncrementalCanSpanMultipleBatches)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db);

    constexpr int64_t order_count = 25;
    create_flx_isolation_schema(seed_db, 0, nullptr, true);
    create_flx_isolation_schema(db, 0, nullptr, false);

    auto sub_store = SubscriptionStore::create(db);
    subscribe_to_flx_isolation_tables(db, sub_store);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    fixture_config.max_download_size = 96;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    std::atomic<bool> count_incremental{false};
    std::mutex batch_mutex;
    int more_to_come_count = 0;
    int last_in_batch_count = 0;

    Session::Config config;
    config.signed_user_token = g_user_0_path_test_token;
    config.user_id = "user_0";
    config.on_sync_client_event_hook = [&](const SyncClientHookData& data) {
        if (!count_incremental.load() || data.event != SyncClientHookEvent::DownloadMessageReceived ||
            data.query_version != 1) {
            return SyncClientHookAction::NoAction;
        }

        std::lock_guard<std::mutex> lock(batch_mutex);
        if (data.batch_state == sync::DownloadBatchState::MoreToCome) {
            ++more_to_come_count;
        }
        else if (data.batch_state == sync::DownloadBatchState::LastInBatch) {
            ++last_in_batch_count;
        }
        return SyncClientHookAction::NoAction;
    };

    util::Optional<Session> session = fixture.make_flx_session(1, 0, db, sub_store, "/test", std::move(config));
    CHECK(session->wait_for_upload_complete_or_client_stopped());
    CHECK(session->wait_for_download_complete_or_client_stopped());
    check_catalog_visible(test_context, db);
    check_order_count(test_context, db, 0);

    count_incremental.store(true);
    add_flx_orders(seed_db, 1, order_count, "user_0");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());
    CHECK(session->wait_for_download_complete_or_client_stopped());

    check_order_count(test_context, db, order_count);
    {
        std::lock_guard<std::mutex> lock(batch_mutex);
        CHECK_GREATER(more_to_come_count, 0);
        CHECK_EQUAL(last_in_batch_count, 1);
    }
}

TEST(Sync_FLXDeletedObjectsLeaveClientView)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    create_flx_isolation_schema(db, 0, nullptr, false);

    auto sub_store = SubscriptionStore::create(db);
    subscribe_to_flx_isolation_tables(db, sub_store);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    Session::Config config;
    config.signed_user_token = g_user_0_path_test_token;
    config.user_id = "user_0";
    util::Optional<Session> session = fixture.make_flx_session(1, 0, db, sub_store, "/test", std::move(config));
    CHECK(session->wait_for_upload_complete_or_client_stopped());
    CHECK(session->wait_for_download_complete_or_client_stopped());
    check_order_count(test_context, db, 1);
    check_catalog_visible(test_context, db);

    delete_flx_order(seed_db, 1);
    delete_flx_catalog_item(seed_db, 10);
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());
    CHECK(session->wait_for_download_complete_or_client_stopped());

    check_order_count(test_context, db, 0);
    check_catalog_count(test_context, db, 0);
}

TEST(Sync_FLXSubscriptionUpdateSendsDeltaAndRemovals)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db);

    constexpr int64_t catalog_count = 25;
    create_flx_isolation_schema(seed_db, 1, "user_0", false);
    add_flx_catalog_items(seed_db, 1, catalog_count);
    create_flx_isolation_schema(db, 0, nullptr, false);

    auto sub_store = SubscriptionStore::create(db);
    set_flx_isolation_subscriptions(db, sub_store, false, true);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    fixture_config.max_download_size = 96;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    std::atomic<bool> count_subscription_update{false};
    std::mutex batch_mutex;
    int more_to_come_count = 0;
    int last_in_batch_count = 0;
    int non_empty_download_count = 0;

    auto reset_counters = [&] {
        std::lock_guard<std::mutex> lock(batch_mutex);
        more_to_come_count = 0;
        last_in_batch_count = 0;
        non_empty_download_count = 0;
    };

    auto check_single_update_batch = [&] {
        std::lock_guard<std::mutex> lock(batch_mutex);
        CHECK_EQUAL(more_to_come_count, 0);
        CHECK_EQUAL(last_in_batch_count, 1);
        CHECK_EQUAL(non_empty_download_count, 1);
    };

    Session::Config config;
    config.signed_user_token = g_user_0_path_test_token;
    config.user_id = "user_0";
    config.on_sync_client_event_hook = [&](const SyncClientHookData& data) {
        if (!count_subscription_update.load() || data.event != SyncClientHookEvent::DownloadMessageReceived ||
            data.query_version <= 1) {
            return SyncClientHookAction::NoAction;
        }

        std::lock_guard<std::mutex> lock(batch_mutex);
        if (data.batch_state == sync::DownloadBatchState::MoreToCome) {
            ++more_to_come_count;
        }
        else if (data.batch_state == sync::DownloadBatchState::LastInBatch) {
            ++last_in_batch_count;
        }
        if (data.num_changesets > 0) {
            ++non_empty_download_count;
        }
        return SyncClientHookAction::NoAction;
    };

    util::Optional<Session> session = fixture.make_flx_session(1, 0, db, sub_store, "/test", std::move(config));
    CHECK(session->wait_for_upload_complete_or_client_stopped());
    CHECK(session->wait_for_download_complete_or_client_stopped());
    check_catalog_count(test_context, db, catalog_count);
    check_order_count(test_context, db, 0);

    count_subscription_update.store(true);
    reset_counters();
    set_flx_isolation_subscriptions(db, sub_store, true, true);
    CHECK(session->wait_for_download_complete_or_client_stopped());
    check_catalog_count(test_context, db, catalog_count);
    check_order_count(test_context, db, 1);
    check_single_update_batch();

    reset_counters();
    set_flx_isolation_subscriptions(db, sub_store, false, true);
    CHECK(session->wait_for_download_complete_or_client_stopped());
    check_catalog_count(test_context, db, catalog_count);
    check_order_count(test_context, db, 0);
    check_single_update_batch();
}

TEST(Sync_FLXDoesNotEchoOwnUploadAsSnapshot)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db);

    create_flx_isolation_schema(seed_db, 0, nullptr, true);
    create_flx_isolation_schema(db, 0, nullptr, false);

    auto sub_store = SubscriptionStore::create(db);
    subscribe_to_flx_isolation_tables(db, sub_store);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    std::atomic<bool> count_after_local_upload{false};
    std::mutex download_mutex;
    int non_empty_download_count = 0;

    Session::Config config;
    config.signed_user_token = g_user_0_path_test_token;
    config.user_id = "user_0";
    config.on_sync_client_event_hook = [&](const SyncClientHookData& data) {
        if (count_after_local_upload.load() && data.event == SyncClientHookEvent::DownloadMessageReceived &&
            data.query_version == 1 && data.num_changesets > 0) {
            std::lock_guard<std::mutex> lock(download_mutex);
            ++non_empty_download_count;
        }
        return SyncClientHookAction::NoAction;
    };

    util::Optional<Session> session = fixture.make_flx_session(1, 0, db, sub_store, "/test", std::move(config));
    CHECK(session->wait_for_upload_complete_or_client_stopped());
    CHECK(session->wait_for_download_complete_or_client_stopped());
    check_catalog_visible(test_context, db);
    check_order_count(test_context, db, 0);

    count_after_local_upload.store(true);
    upsert_order_owner(db, 1, "user_0");
    CHECK(session->wait_for_upload_complete_or_client_stopped());
    CHECK(session->wait_for_download_complete_or_client_stopped());

    check_order_count(test_context, db, 1);
    {
        std::lock_guard<std::mutex> lock(download_mutex);
        CHECK_EQUAL(non_empty_download_count, 0);
    }
}

TEST(Sync_FLXUnauthorizedOwnerWriteCreatesCompensatingWrite)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);
    TEST_CLIENT_DB(db_b);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    upsert_order_owner(seed_db, 2, "user_1");
    create_flx_isolation_schema(db_a, 0, nullptr, false);
    create_flx_isolation_schema(db_b, 0, nullptr, false);

    auto sub_store_a = SubscriptionStore::create(db_a);
    auto sub_store_b = SubscriptionStore::create(db_b);
    subscribe_to_flx_isolation_tables(db_a, sub_store_a);
    subscribe_to_flx_isolation_tables(db_b, sub_store_b);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(3, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    std::mutex error_mutex;
    bool got_compensating_write = false;

    Session::Config config_a;
    config_a.signed_user_token = g_user_0_path_test_token;
    config_a.user_id = "user_0";
    config_a.connection_state_change_listener = [&](ConnectionState state,
                                                    std::optional<SessionErrorInfo> error_info) {
        if (error_info && error_info->status == ErrorCodes::SyncCompensatingWrite) {
            std::lock_guard<std::mutex> lock(error_mutex);
            CHECK_EQUAL(state, ConnectionState::connected);
            CHECK_NOT(error_info->is_fatal);
            CHECK_EQUAL(error_info->compensating_writes.size(), 1);
            const auto& rejected_update = error_info->compensating_writes.front();
            CHECK_EQUAL(rejected_update.object_name, "Order");
            CHECK_EQUAL(rejected_update.primary_key.get_int(), 1);
            CHECK(StringData{rejected_update.reason}.contains("owner field"));
            got_compensating_write = true;
            return;
        }
        if (state == ConnectionState::disconnected && error_info) {
            test_context.logger->error("Client disconnect: %1 (is_fatal=%2)", error_info->status,
                                       error_info->is_fatal);
            bool client_error_occurred = true;
            CHECK_NOT(client_error_occurred);
            fixture.stop();
        }
    };

    util::Optional<Session> session_a =
        fixture.make_flx_session(1, 0, db_a, sub_store_a, "/test", std::move(config_a));
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    Session::Config config_b;
    config_b.signed_user_token = g_user_1_path_test_token;
    config_b.user_id = "user_1";
    util::Optional<Session> session_b =
        fixture.make_flx_session(2, 0, db_b, sub_store_b, "/test", std::move(config_b));
    CHECK(session_b->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());
    CHECK(session_b->wait_for_download_complete_or_client_stopped());

    check_order_visibility(test_context, db_a, true, false);
    check_order_visibility(test_context, db_b, false, true);

    upsert_order_owner(db_a, 1, "user_1");
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());
    CHECK(session_b->wait_for_download_complete_or_client_stopped());

    {
        std::lock_guard<std::mutex> lock(error_mutex);
        CHECK(got_compensating_write);
    }
    check_order_visibility(test_context, db_a, true, false);
    check_order_owner(test_context, db_a, 1, "user_0");
    check_order_visibility(test_context, db_b, false, true);
}

TEST(Sync_FLXCompensatingWriteKeepsAllowedObjectsFromSameUpload)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);
    TEST_CLIENT_DB(db_b);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    create_flx_isolation_schema(db_a, 0, nullptr, false);
    create_flx_isolation_schema(db_b, 0, nullptr, false);

    auto sub_store_a = SubscriptionStore::create(db_a);
    auto sub_store_b = SubscriptionStore::create(db_b);
    subscribe_to_flx_isolation_tables(db_a, sub_store_a);
    subscribe_to_flx_isolation_tables(db_b, sub_store_b);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(3, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    std::mutex error_mutex;
    bool got_compensating_write = false;

    Session::Config config_a;
    config_a.signed_user_token = g_user_0_path_test_token;
    config_a.user_id = "user_0";
    config_a.connection_state_change_listener = [&](ConnectionState state,
                                                    std::optional<SessionErrorInfo> error_info) {
        if (error_info && error_info->status == ErrorCodes::SyncCompensatingWrite) {
            std::lock_guard<std::mutex> lock(error_mutex);
            CHECK_EQUAL(state, ConnectionState::connected);
            CHECK_NOT(error_info->is_fatal);
            CHECK_EQUAL(error_info->compensating_writes.size(), 1);
            const auto& rejected_update = error_info->compensating_writes.front();
            CHECK_EQUAL(rejected_update.object_name, "Order");
            CHECK_EQUAL(rejected_update.primary_key.get_int(), 3);
            CHECK(StringData{rejected_update.reason}.contains("owner field"));
            got_compensating_write = true;
            return;
        }
        if (state == ConnectionState::disconnected && error_info) {
            test_context.logger->error("Client disconnect: %1 (is_fatal=%2)", error_info->status,
                                       error_info->is_fatal);
            bool client_error_occurred = true;
            CHECK_NOT(client_error_occurred);
            fixture.stop();
        }
    };

    util::Optional<Session> session_a =
        fixture.make_flx_session(1, 0, db_a, sub_store_a, "/test", std::move(config_a));
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    Session::Config config_b;
    config_b.signed_user_token = g_user_0_path_test_token;
    config_b.user_id = "user_0";
    util::Optional<Session> session_b =
        fixture.make_flx_session(2, 0, db_b, sub_store_b, "/test", std::move(config_b));
    CHECK(session_b->wait_for_upload_complete_or_client_stopped());
    CHECK(session_b->wait_for_download_complete_or_client_stopped());

    check_order_count(test_context, db_a, 1);
    check_order_count(test_context, db_b, 1);

    update_order_item_and_create_order(db_a, 1, "allowed-edit", 3, "user_1");
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());
    CHECK(session_b->wait_for_download_complete_or_client_stopped());

    {
        std::lock_guard<std::mutex> lock(error_mutex);
        CHECK(got_compensating_write);
    }
    check_order_count(test_context, db_a, 1);
    check_order_count(test_context, db_b, 1);
    check_order_item(test_context, db_a, 1, "allowed-edit");
    check_order_item(test_context, db_b, 1, "allowed-edit");
    check_order_exists(test_context, db_a, 3, false);
    check_order_exists(test_context, db_b, 3, false);
}

TEST(Sync_FLXOwnerWriteCannotInjectIntoAnotherUsersView)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);
    TEST_CLIENT_DB(db_b);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    upsert_order_owner(seed_db, 2, "user_1");
    create_flx_isolation_schema(db_a, 0, nullptr, false);
    create_flx_isolation_schema(db_b, 0, nullptr, false);

    auto sub_store_a = SubscriptionStore::create(db_a);
    auto sub_store_b = SubscriptionStore::create(db_b);
    subscribe_to_flx_isolation_tables(db_a, sub_store_a);
    subscribe_to_flx_isolation_tables(db_b, sub_store_b);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(3, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    std::mutex error_mutex;
    bool got_compensating_write = false;

    Session::Config config_a;
    config_a.signed_user_token = g_user_0_path_test_token;
    config_a.user_id = "user_0";
    config_a.connection_state_change_listener = [&](ConnectionState state,
                                                    std::optional<SessionErrorInfo> error_info) {
        if (error_info && error_info->status == ErrorCodes::SyncCompensatingWrite) {
            std::lock_guard<std::mutex> lock(error_mutex);
            CHECK_EQUAL(state, ConnectionState::connected);
            CHECK_EQUAL(error_info->compensating_writes.size(), 1);
            got_compensating_write = true;
            return;
        }
        if (state == ConnectionState::disconnected && error_info) {
            test_context.logger->error("Client disconnect: %1 (is_fatal=%2)", error_info->status,
                                       error_info->is_fatal);
            bool client_error_occurred = true;
            CHECK_NOT(client_error_occurred);
            fixture.stop();
        }
    };

    util::Optional<Session> session_a =
        fixture.make_flx_session(1, 0, db_a, sub_store_a, "/test", std::move(config_a));
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    Session::Config config_b;
    config_b.signed_user_token = g_user_1_path_test_token;
    config_b.user_id = "user_1";
    util::Optional<Session> session_b =
        fixture.make_flx_session(2, 0, db_b, sub_store_b, "/test", std::move(config_b));
    CHECK(session_b->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());
    CHECK(session_b->wait_for_download_complete_or_client_stopped());

    check_order_visibility(test_context, db_a, true, false);
    check_order_visibility(test_context, db_b, false, true);

    upsert_order_owner(db_a, 3, "user_1");
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());
    CHECK(session_b->wait_for_download_complete_or_client_stopped());

    {
        std::lock_guard<std::mutex> lock(error_mutex);
        CHECK(got_compensating_write);
    }
    check_order_exists(test_context, db_a, 3, false);
    check_order_exists(test_context, db_b, 3, false);
    check_order_visibility(test_context, db_b, false, true);
}

TEST(Sync_FLXPublicReadOnlyWriteCreatesCompensatingWrite)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    create_flx_isolation_schema(db_a, 0, nullptr, false);

    auto sub_store_a = SubscriptionStore::create(db_a);
    subscribe_to_flx_isolation_tables(db_a, sub_store_a);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    std::mutex error_mutex;
    bool got_compensating_write = false;

    Session::Config config_a;
    config_a.signed_user_token = g_user_0_path_test_token;
    config_a.user_id = "user_0";
    config_a.connection_state_change_listener = [&](ConnectionState state,
                                                    std::optional<SessionErrorInfo> error_info) {
        if (error_info && error_info->status == ErrorCodes::SyncCompensatingWrite) {
            std::lock_guard<std::mutex> lock(error_mutex);
            CHECK_EQUAL(state, ConnectionState::connected);
            CHECK_EQUAL(error_info->compensating_writes.size(), 1);
            got_compensating_write = true;
            return;
        }
        if (state == ConnectionState::disconnected && error_info) {
            test_context.logger->error("Client disconnect: %1 (is_fatal=%2)", error_info->status,
                                       error_info->is_fatal);
            bool client_error_occurred = true;
            CHECK_NOT(client_error_occurred);
            fixture.stop();
        }
    };

    util::Optional<Session> session_a =
        fixture.make_flx_session(1, 0, db_a, sub_store_a, "/test", std::move(config_a));
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    check_catalog_visible(test_context, db_a);
    check_catalog_name(test_context, db_a, 10, "shared");

    set_flx_catalog_name(db_a, 10, "hacked");
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    {
        std::lock_guard<std::mutex> lock(error_mutex);
        CHECK(got_compensating_write);
    }
    check_catalog_visible(test_context, db_a);
    check_catalog_name(test_context, db_a, 10, "shared");
}

TEST(Sync_FLXPublicReadOnlyDeleteCreatesCompensatingWrite)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    create_flx_isolation_schema(db_a, 0, nullptr, false);

    auto sub_store_a = SubscriptionStore::create(db_a);
    subscribe_to_flx_isolation_tables(db_a, sub_store_a);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    std::mutex error_mutex;
    bool got_compensating_write = false;

    Session::Config config_a;
    config_a.signed_user_token = g_user_0_path_test_token;
    config_a.user_id = "user_0";
    config_a.connection_state_change_listener = [&](ConnectionState state,
                                                    std::optional<SessionErrorInfo> error_info) {
        if (error_info && error_info->status == ErrorCodes::SyncCompensatingWrite) {
            std::lock_guard<std::mutex> lock(error_mutex);
            CHECK_EQUAL(state, ConnectionState::connected);
            CHECK_EQUAL(error_info->compensating_writes.size(), 1);
            got_compensating_write = true;
            return;
        }
        if (state == ConnectionState::disconnected && error_info) {
            test_context.logger->error("Client disconnect: %1 (is_fatal=%2)", error_info->status,
                                       error_info->is_fatal);
            bool client_error_occurred = true;
            CHECK_NOT(client_error_occurred);
            fixture.stop();
        }
    };

    util::Optional<Session> session_a =
        fixture.make_flx_session(1, 0, db_a, sub_store_a, "/test", std::move(config_a));
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    check_catalog_visible(test_context, db_a);
    check_catalog_name(test_context, db_a, 10, "shared");

    delete_flx_catalog_item(db_a, 10);
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    {
        std::lock_guard<std::mutex> lock(error_mutex);
        CHECK(got_compensating_write);
    }
    check_catalog_visible(test_context, db_a);
    check_catalog_name(test_context, db_a, 10, "shared");
}

TEST(Sync_FLXPublicReadOnlyCreateCreatesCompensatingWrite)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    create_flx_isolation_schema(db_a, 0, nullptr, false);

    auto sub_store_a = SubscriptionStore::create(db_a);
    subscribe_to_flx_isolation_tables(db_a, sub_store_a);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    std::mutex error_mutex;
    bool got_compensating_write = false;

    Session::Config config_a;
    config_a.signed_user_token = g_user_0_path_test_token;
    config_a.user_id = "user_0";
    config_a.connection_state_change_listener = [&](ConnectionState state,
                                                    std::optional<SessionErrorInfo> error_info) {
        if (error_info && error_info->status == ErrorCodes::SyncCompensatingWrite) {
            std::lock_guard<std::mutex> lock(error_mutex);
            CHECK_EQUAL(state, ConnectionState::connected);
            CHECK_EQUAL(error_info->compensating_writes.size(), 1);
            got_compensating_write = true;
            return;
        }
        if (state == ConnectionState::disconnected && error_info) {
            test_context.logger->error("Client disconnect: %1 (is_fatal=%2)", error_info->status,
                                       error_info->is_fatal);
            bool client_error_occurred = true;
            CHECK_NOT(client_error_occurred);
            fixture.stop();
        }
    };

    util::Optional<Session> session_a =
        fixture.make_flx_session(1, 0, db_a, sub_store_a, "/test", std::move(config_a));
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    check_catalog_visible(test_context, db_a);
    check_catalog_name(test_context, db_a, 10, "shared");

    set_flx_catalog_name(db_a, 11, "bad-create");
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    {
        std::lock_guard<std::mutex> lock(error_mutex);
        CHECK(got_compensating_write);
    }
    check_catalog_count(test_context, db_a, 1);
    check_catalog_name(test_context, db_a, 10, "shared");
}

TEST(Sync_FLXPublicReadOnlyListWriteCreatesCompensatingWrite)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    add_flx_catalog_scores_schema(seed_db, {1, 2});
    create_flx_isolation_schema(db_a, 0, nullptr, false);
    add_flx_catalog_scores_schema(db_a, {});

    auto sub_store_a = SubscriptionStore::create(db_a);
    subscribe_to_flx_isolation_tables(db_a, sub_store_a);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    std::mutex error_mutex;
    bool got_compensating_write = false;

    Session::Config config_a;
    config_a.signed_user_token = g_user_0_path_test_token;
    config_a.user_id = "user_0";
    config_a.connection_state_change_listener = [&](ConnectionState state,
                                                    std::optional<SessionErrorInfo> error_info) {
        if (error_info && error_info->status == ErrorCodes::SyncCompensatingWrite) {
            std::lock_guard<std::mutex> lock(error_mutex);
            CHECK_EQUAL(state, ConnectionState::connected);
            CHECK_EQUAL(error_info->compensating_writes.size(), 1);
            got_compensating_write = true;
            return;
        }
        if (state == ConnectionState::disconnected && error_info) {
            test_context.logger->error("Client disconnect: %1 (is_fatal=%2)", error_info->status,
                                       error_info->is_fatal);
            bool client_error_occurred = true;
            CHECK_NOT(client_error_occurred);
            fixture.stop();
        }
    };

    util::Optional<Session> session_a =
        fixture.make_flx_session(1, 0, db_a, sub_store_a, "/test", std::move(config_a));
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    check_catalog_visible(test_context, db_a);
    check_catalog_name(test_context, db_a, 10, "shared");
    check_catalog_scores(test_context, db_a, {1, 2});

    set_flx_catalog_scores(db_a, {99});
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    {
        std::lock_guard<std::mutex> lock(error_mutex);
        CHECK(got_compensating_write);
    }
    check_catalog_scores(test_context, db_a, {1, 2});
}

TEST(Sync_FLXPublicReadOnlySetWriteCreatesCompensatingWrite)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    add_flx_catalog_score_set_schema(seed_db, {1, 2});
    create_flx_isolation_schema(db_a, 0, nullptr, false);
    add_flx_catalog_score_set_schema(db_a, {});

    auto sub_store_a = SubscriptionStore::create(db_a);
    subscribe_to_flx_isolation_tables(db_a, sub_store_a);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    std::mutex error_mutex;
    bool got_compensating_write = false;

    Session::Config config_a;
    config_a.signed_user_token = g_user_0_path_test_token;
    config_a.user_id = "user_0";
    config_a.connection_state_change_listener = [&](ConnectionState state,
                                                    std::optional<SessionErrorInfo> error_info) {
        if (error_info && error_info->status == ErrorCodes::SyncCompensatingWrite) {
            std::lock_guard<std::mutex> lock(error_mutex);
            CHECK_EQUAL(state, ConnectionState::connected);
            CHECK_EQUAL(error_info->compensating_writes.size(), 1);
            got_compensating_write = true;
            return;
        }
        if (state == ConnectionState::disconnected && error_info) {
            test_context.logger->error("Client disconnect: %1 (is_fatal=%2)", error_info->status,
                                       error_info->is_fatal);
            bool client_error_occurred = true;
            CHECK_NOT(client_error_occurred);
            fixture.stop();
        }
    };

    util::Optional<Session> session_a =
        fixture.make_flx_session(1, 0, db_a, sub_store_a, "/test", std::move(config_a));
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    check_catalog_visible(test_context, db_a);
    check_catalog_name(test_context, db_a, 10, "shared");
    check_catalog_score_set(test_context, db_a, {1, 2});

    set_flx_catalog_score_set(db_a, {99});
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    {
        std::lock_guard<std::mutex> lock(error_mutex);
        CHECK(got_compensating_write);
    }
    check_catalog_score_set(test_context, db_a, {1, 2});
}

TEST(Sync_FLXPublicReadOnlyDictionaryWriteCreatesCompensatingWrite)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    add_flx_catalog_note_dict_schema(seed_db, {{"first", "one"}, {"second", "two"}});
    create_flx_isolation_schema(db_a, 0, nullptr, false);
    add_flx_catalog_note_dict_schema(db_a, {});

    auto sub_store_a = SubscriptionStore::create(db_a);
    subscribe_to_flx_isolation_tables(db_a, sub_store_a);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    std::mutex error_mutex;
    bool got_compensating_write = false;

    Session::Config config_a;
    config_a.signed_user_token = g_user_0_path_test_token;
    config_a.user_id = "user_0";
    config_a.connection_state_change_listener = [&](ConnectionState state,
                                                    std::optional<SessionErrorInfo> error_info) {
        if (error_info && error_info->status == ErrorCodes::SyncCompensatingWrite) {
            std::lock_guard<std::mutex> lock(error_mutex);
            CHECK_EQUAL(state, ConnectionState::connected);
            CHECK_EQUAL(error_info->compensating_writes.size(), 1);
            got_compensating_write = true;
            return;
        }
        if (state == ConnectionState::disconnected && error_info) {
            test_context.logger->error("Client disconnect: %1 (is_fatal=%2)", error_info->status,
                                       error_info->is_fatal);
            bool client_error_occurred = true;
            CHECK_NOT(client_error_occurred);
            fixture.stop();
        }
    };

    util::Optional<Session> session_a =
        fixture.make_flx_session(1, 0, db_a, sub_store_a, "/test", std::move(config_a));
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    check_catalog_visible(test_context, db_a);
    check_catalog_name(test_context, db_a, 10, "shared");
    check_catalog_note_dict(test_context, db_a, {{"first", "one"}, {"second", "two"}});

    set_flx_catalog_note_dict(db_a, {{"first", "hacked"}});
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    {
        std::lock_guard<std::mutex> lock(error_mutex);
        CHECK(got_compensating_write);
    }
    check_catalog_note_dict(test_context, db_a, {{"first", "one"}, {"second", "two"}});
}

TEST(Sync_FLXPublicReadOnlyMixedWriteCreatesCompensatingWrite)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    add_flx_catalog_mixed_schema(seed_db, true);
    create_flx_isolation_schema(db_a, 0, nullptr, false);
    add_flx_catalog_mixed_schema(db_a, false);

    auto sub_store_a = SubscriptionStore::create(db_a);
    subscribe_to_flx_isolation_tables(db_a, sub_store_a);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    std::mutex error_mutex;
    bool got_compensating_write = false;

    Session::Config config_a;
    config_a.signed_user_token = g_user_0_path_test_token;
    config_a.user_id = "user_0";
    config_a.connection_state_change_listener = [&](ConnectionState state,
                                                    std::optional<SessionErrorInfo> error_info) {
        if (error_info && error_info->status == ErrorCodes::SyncCompensatingWrite) {
            std::lock_guard<std::mutex> lock(error_mutex);
            CHECK_EQUAL(state, ConnectionState::connected);
            CHECK_EQUAL(error_info->compensating_writes.size(), 1);
            got_compensating_write = true;
            return;
        }
        if (state == ConnectionState::disconnected && error_info) {
            test_context.logger->error("Client disconnect: %1 (is_fatal=%2)", error_info->status,
                                       error_info->is_fatal);
            bool client_error_occurred = true;
            CHECK_NOT(client_error_occurred);
            fixture.stop();
        }
    };

    util::Optional<Session> session_a =
        fixture.make_flx_session(1, 0, db_a, sub_store_a, "/test", std::move(config_a));
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    check_catalog_visible(test_context, db_a);
    check_catalog_name(test_context, db_a, 10, "shared");
    check_catalog_mixed_values(test_context, db_a);

    set_flx_catalog_mixed_hacked(db_a);
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    {
        std::lock_guard<std::mutex> lock(error_mutex);
        CHECK(got_compensating_write);
    }
    check_catalog_mixed_values(test_context, db_a);
}

TEST(Sync_FLXPublicReadOnlyMixedLinksWriteCreatesCompensatingWrite)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    add_flx_catalog_mixed_link_schema(seed_db, true);
    create_flx_isolation_schema(db_a, 0, nullptr, false);
    add_flx_catalog_mixed_link_schema(db_a, false);

    auto sub_store_a = SubscriptionStore::create(db_a);
    subscribe_to_flx_isolation_tables_with_category(db_a, sub_store_a);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});
    fixture_config.flx_rules.push_back({"Category", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    std::mutex error_mutex;
    bool got_compensating_write = false;

    Session::Config config_a;
    config_a.signed_user_token = g_user_0_path_test_token;
    config_a.user_id = "user_0";
    config_a.connection_state_change_listener = [&](ConnectionState state,
                                                    std::optional<SessionErrorInfo> error_info) {
        if (error_info && error_info->status == ErrorCodes::SyncCompensatingWrite) {
            std::lock_guard<std::mutex> lock(error_mutex);
            CHECK_EQUAL(state, ConnectionState::connected);
            CHECK_EQUAL(error_info->compensating_writes.size(), 1);
            got_compensating_write = true;
            return;
        }
        if (state == ConnectionState::disconnected && error_info) {
            test_context.logger->error("Client disconnect: %1 (is_fatal=%2)", error_info->status,
                                       error_info->is_fatal);
            bool client_error_occurred = true;
            CHECK_NOT(client_error_occurred);
            fixture.stop();
        }
    };

    util::Optional<Session> session_a =
        fixture.make_flx_session(1, 0, db_a, sub_store_a, "/test", std::move(config_a));
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    check_catalog_visible(test_context, db_a);
    check_catalog_name(test_context, db_a, 10, "shared");
    check_catalog_mixed_links(test_context, db_a);

    set_flx_catalog_mixed_links_to_second(db_a);
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    {
        std::lock_guard<std::mutex> lock(error_mutex);
        CHECK(got_compensating_write);
    }
    check_catalog_mixed_links(test_context, db_a);
}

TEST(Sync_FLXPublicReadOnlyLinkWriteCreatesCompensatingWrite)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    add_flx_catalog_category_schema(seed_db, true);
    create_flx_isolation_schema(db_a, 0, nullptr, false);
    add_flx_catalog_category_schema(db_a, false);

    auto sub_store_a = SubscriptionStore::create(db_a);
    subscribe_to_flx_isolation_tables_with_category(db_a, sub_store_a);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});
    fixture_config.flx_rules.push_back({"Category", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    std::mutex error_mutex;
    bool got_compensating_write = false;

    Session::Config config_a;
    config_a.signed_user_token = g_user_0_path_test_token;
    config_a.user_id = "user_0";
    config_a.connection_state_change_listener = [&](ConnectionState state,
                                                    std::optional<SessionErrorInfo> error_info) {
        if (error_info && error_info->status == ErrorCodes::SyncCompensatingWrite) {
            std::lock_guard<std::mutex> lock(error_mutex);
            CHECK_EQUAL(state, ConnectionState::connected);
            CHECK_EQUAL(error_info->compensating_writes.size(), 1);
            got_compensating_write = true;
            return;
        }
        if (state == ConnectionState::disconnected && error_info) {
            test_context.logger->error("Client disconnect: %1 (is_fatal=%2)", error_info->status,
                                       error_info->is_fatal);
            bool client_error_occurred = true;
            CHECK_NOT(client_error_occurred);
            fixture.stop();
        }
    };

    util::Optional<Session> session_a =
        fixture.make_flx_session(1, 0, db_a, sub_store_a, "/test", std::move(config_a));
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    check_catalog_visible(test_context, db_a);
    check_catalog_name(test_context, db_a, 10, "shared");
    check_catalog_category_label(test_context, db_a, "tools");

    set_flx_catalog_category_null(db_a);
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    {
        std::lock_guard<std::mutex> lock(error_mutex);
        CHECK(got_compensating_write);
    }
    check_catalog_category_label(test_context, db_a, "tools");
}

TEST(Sync_FLXLinkTargetOutsideSubscriptionStaysUnresolvedUntilSubscribed)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    add_flx_catalog_category_schema(seed_db, true);
    create_flx_isolation_schema(db_a, 0, nullptr, false);
    add_flx_catalog_category_schema(db_a, false);

    auto sub_store_a = SubscriptionStore::create(db_a);
    set_flx_isolation_subscriptions(db_a, sub_store_a, false, true);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});
    fixture_config.flx_rules.push_back({"Category", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    Session::Config config_a;
    config_a.signed_user_token = g_user_0_path_test_token;
    config_a.user_id = "user_0";
    util::Optional<Session> session_a =
        fixture.make_flx_session(1, 0, db_a, sub_store_a, "/test", std::move(config_a));
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    check_catalog_visible(test_context, db_a);
    check_catalog_name(test_context, db_a, 10, "shared");
    check_catalog_category_unresolved(test_context, db_a);

    subscribe_to_flx_isolation_tables_with_category(db_a, sub_store_a);
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    check_catalog_category_label(test_context, db_a, "tools");
}

TEST(Sync_FLXMixedLinkTargetsOutsideSubscriptionResolveWhenSubscribed)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    add_flx_catalog_mixed_link_schema(seed_db, true);
    create_flx_isolation_schema(db_a, 0, nullptr, false);
    add_flx_catalog_mixed_link_schema(db_a, false);

    auto sub_store_a = SubscriptionStore::create(db_a);
    set_flx_isolation_subscriptions(db_a, sub_store_a, false, true);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});
    fixture_config.flx_rules.push_back({"Category", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    Session::Config config_a;
    config_a.signed_user_token = g_user_0_path_test_token;
    config_a.user_id = "user_0";
    util::Optional<Session> session_a =
        fixture.make_flx_session(1, 0, db_a, sub_store_a, "/test", std::move(config_a));
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    check_catalog_visible(test_context, db_a);
    check_catalog_name(test_context, db_a, 10, "shared");
    check_catalog_mixed_links_pending(test_context, db_a);

    subscribe_to_flx_isolation_tables_with_category(db_a, sub_store_a);
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    check_catalog_mixed_links(test_context, db_a);
}

TEST(Sync_FLXPublicReadOnlyLinkCollectionsWriteCreatesCompensatingWrite)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    add_flx_catalog_category_collections_schema(seed_db, true);
    create_flx_isolation_schema(db_a, 0, nullptr, false);
    add_flx_catalog_category_collections_schema(db_a, false);

    auto sub_store_a = SubscriptionStore::create(db_a);
    subscribe_to_flx_isolation_tables_with_category(db_a, sub_store_a);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});
    fixture_config.flx_rules.push_back({"Category", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    std::mutex error_mutex;
    bool got_compensating_write = false;

    Session::Config config_a;
    config_a.signed_user_token = g_user_0_path_test_token;
    config_a.user_id = "user_0";
    config_a.connection_state_change_listener = [&](ConnectionState state,
                                                    std::optional<SessionErrorInfo> error_info) {
        if (error_info && error_info->status == ErrorCodes::SyncCompensatingWrite) {
            std::lock_guard<std::mutex> lock(error_mutex);
            CHECK_EQUAL(state, ConnectionState::connected);
            CHECK_EQUAL(error_info->compensating_writes.size(), 1);
            got_compensating_write = true;
            return;
        }
        if (state == ConnectionState::disconnected && error_info) {
            test_context.logger->error("Client disconnect: %1 (is_fatal=%2)", error_info->status,
                                       error_info->is_fatal);
            bool client_error_occurred = true;
            CHECK_NOT(client_error_occurred);
            fixture.stop();
        }
    };

    util::Optional<Session> session_a =
        fixture.make_flx_session(1, 0, db_a, sub_store_a, "/test", std::move(config_a));
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    check_catalog_visible(test_context, db_a);
    check_catalog_name(test_context, db_a, 10, "shared");
    check_catalog_category_collections(test_context, db_a);

    set_flx_catalog_category_collections_to_second(db_a);
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    {
        std::lock_guard<std::mutex> lock(error_mutex);
        CHECK(got_compensating_write);
    }
    check_catalog_category_collections(test_context, db_a);
}

TEST(Sync_FLXPublicReadOnlyEmbeddedObjectWriteCreatesCompensatingWrite)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    add_flx_catalog_detail_schema(seed_db, true);
    create_flx_isolation_schema(db_a, 0, nullptr, false);
    add_flx_catalog_detail_schema(db_a, false);

    auto sub_store_a = SubscriptionStore::create(db_a);
    subscribe_to_flx_isolation_tables(db_a, sub_store_a);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    std::mutex error_mutex;
    bool got_compensating_write = false;

    Session::Config config_a;
    config_a.signed_user_token = g_user_0_path_test_token;
    config_a.user_id = "user_0";
    config_a.connection_state_change_listener = [&](ConnectionState state,
                                                    std::optional<SessionErrorInfo> error_info) {
        if (error_info && error_info->status == ErrorCodes::SyncCompensatingWrite) {
            std::lock_guard<std::mutex> lock(error_mutex);
            CHECK_EQUAL(state, ConnectionState::connected);
            CHECK_EQUAL(error_info->compensating_writes.size(), 1);
            got_compensating_write = true;
            return;
        }
        if (state == ConnectionState::disconnected && error_info) {
            test_context.logger->error("Client disconnect: %1 (is_fatal=%2)", error_info->status,
                                       error_info->is_fatal);
            bool client_error_occurred = true;
            CHECK_NOT(client_error_occurred);
            fixture.stop();
        }
    };

    util::Optional<Session> session_a =
        fixture.make_flx_session(1, 0, db_a, sub_store_a, "/test", std::move(config_a));
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    check_catalog_visible(test_context, db_a);
    check_catalog_name(test_context, db_a, 10, "shared");
    check_catalog_detail(test_context, db_a, "seeded", 5);

    set_flx_catalog_detail_summary(db_a, "hacked");
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    {
        std::lock_guard<std::mutex> lock(error_mutex);
        CHECK(got_compensating_write);
    }
    check_catalog_detail(test_context, db_a, "seeded", 5);
}

TEST(Sync_FLXPublicReadOnlyEmbeddedCollectionsWriteCreatesCompensatingWrite)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    add_flx_catalog_detail_collections_schema(seed_db, true);
    create_flx_isolation_schema(db_a, 0, nullptr, false);
    add_flx_catalog_detail_collections_schema(db_a, false);

    auto sub_store_a = SubscriptionStore::create(db_a);
    subscribe_to_flx_isolation_tables(db_a, sub_store_a);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    std::mutex error_mutex;
    bool got_compensating_write = false;

    Session::Config config_a;
    config_a.signed_user_token = g_user_0_path_test_token;
    config_a.user_id = "user_0";
    config_a.connection_state_change_listener = [&](ConnectionState state,
                                                    std::optional<SessionErrorInfo> error_info) {
        if (error_info && error_info->status == ErrorCodes::SyncCompensatingWrite) {
            std::lock_guard<std::mutex> lock(error_mutex);
            CHECK_EQUAL(state, ConnectionState::connected);
            CHECK_EQUAL(error_info->compensating_writes.size(), 1);
            got_compensating_write = true;
            return;
        }
        if (state == ConnectionState::disconnected && error_info) {
            test_context.logger->error("Client disconnect: %1 (is_fatal=%2)", error_info->status,
                                       error_info->is_fatal);
            bool client_error_occurred = true;
            CHECK_NOT(client_error_occurred);
            fixture.stop();
        }
    };

    util::Optional<Session> session_a =
        fixture.make_flx_session(1, 0, db_a, sub_store_a, "/test", std::move(config_a));
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    check_catalog_visible(test_context, db_a);
    check_catalog_name(test_context, db_a, 10, "shared");
    check_catalog_detail_collections(test_context, db_a);

    set_flx_catalog_detail_collections_hacked(db_a);
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    {
        std::lock_guard<std::mutex> lock(error_mutex);
        CHECK(got_compensating_write);
    }
    check_catalog_detail_collections(test_context, db_a);
}

TEST(Sync_FLXAdminCanReadAndWriteConfiguredTables)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_admin);
    TEST_CLIENT_DB(db_user_0);
    TEST_CLIENT_DB(db_user_1);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    upsert_order_owner(seed_db, 2, "user_1");
    create_flx_isolation_schema(db_admin, 0, nullptr, false);
    create_flx_isolation_schema(db_user_0, 0, nullptr, false);
    create_flx_isolation_schema(db_user_1, 0, nullptr, false);

    auto sub_store_admin = SubscriptionStore::create(db_admin);
    auto sub_store_user_0 = SubscriptionStore::create(db_user_0);
    auto sub_store_user_1 = SubscriptionStore::create(db_user_1);
    subscribe_to_flx_isolation_tables(db_admin, sub_store_admin);
    subscribe_to_flx_isolation_tables(db_user_0, sub_store_user_0);
    subscribe_to_flx_isolation_tables(db_user_1, sub_store_user_1);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(4, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    std::mutex error_mutex;
    bool got_compensating_write = false;

    Session::Config admin_config;
    admin_config.signed_user_token = g_admin_path_test_token;
    admin_config.user_id = "admin";
    admin_config.connection_state_change_listener = [&](ConnectionState state,
                                                        std::optional<SessionErrorInfo> error_info) {
        if (error_info && error_info->status == ErrorCodes::SyncCompensatingWrite) {
            std::lock_guard<std::mutex> lock(error_mutex);
            CHECK_EQUAL(state, ConnectionState::connected);
            got_compensating_write = true;
            return;
        }
        if (state == ConnectionState::disconnected && error_info) {
            test_context.logger->error("Admin disconnect: %1 (is_fatal=%2)", error_info->status,
                                       error_info->is_fatal);
            bool client_error_occurred = true;
            CHECK_NOT(client_error_occurred);
            fixture.stop();
        }
    };

    util::Optional<Session> admin_session =
        fixture.make_flx_session(1, 0, db_admin, sub_store_admin, "/test", std::move(admin_config));
    CHECK(admin_session->wait_for_upload_complete_or_client_stopped());
    CHECK(admin_session->wait_for_download_complete_or_client_stopped());

    check_order_visibility(test_context, db_admin, true, true);
    check_catalog_name(test_context, db_admin, 10, "shared");

    upsert_order_owner(db_admin, 1, "user_1");
    set_flx_catalog_name(db_admin, 10, "admin-edit");
    CHECK(admin_session->wait_for_upload_complete_or_client_stopped());
    CHECK(admin_session->wait_for_download_complete_or_client_stopped());

    Session::Config user_0_config;
    user_0_config.signed_user_token = g_user_0_path_test_token;
    user_0_config.user_id = "user_0";
    util::Optional<Session> user_0_session =
        fixture.make_flx_session(2, 0, db_user_0, sub_store_user_0, "/test", std::move(user_0_config));

    Session::Config user_1_config;
    user_1_config.signed_user_token = g_user_1_path_test_token;
    user_1_config.user_id = "user_1";
    util::Optional<Session> user_1_session =
        fixture.make_flx_session(3, 0, db_user_1, sub_store_user_1, "/test", std::move(user_1_config));

    CHECK(user_0_session->wait_for_upload_complete_or_client_stopped());
    CHECK(user_1_session->wait_for_upload_complete_or_client_stopped());
    CHECK(user_0_session->wait_for_download_complete_or_client_stopped());
    CHECK(user_1_session->wait_for_download_complete_or_client_stopped());

    {
        std::lock_guard<std::mutex> lock(error_mutex);
        CHECK_NOT(got_compensating_write);
    }
    check_order_visibility(test_context, db_user_0, false, false);
    check_order_visibility(test_context, db_user_1, true, true);
    check_catalog_name(test_context, db_user_0, 10, "admin-edit");
    check_catalog_name(test_context, db_user_1, 10, "admin-edit");
}

TEST(Sync_FLXDefaultDenyWriteCreatesCompensatingWrite)
{
    TEST_DIR(dir);
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_a);

    create_flx_isolation_schema(seed_db, 1, "user_0", true);
    add_flx_private_schema(seed_db);
    create_flx_isolation_schema(db_a, 0, nullptr, false);
    add_flx_private_schema(db_a);

    auto sub_store_a = SubscriptionStore::create(db_a);
    subscribe_to_flx_isolation_tables(db_a, sub_store_a);

    MultiClientServerFixture::Config fixture_config;
    fixture_config.enable_flx_sync = true;
    using FLXRule = Server::Config::FLXRule;
    fixture_config.flx_rules.push_back({"Order", FLXRule::Mode::Owner, "owner_id"});
    fixture_config.flx_rules.push_back({"Catalog", FLXRule::Mode::PublicReadOnly, ""});

    MultiClientServerFixture fixture(2, 1, dir, test_context, std::move(fixture_config));
    fixture.start();

    util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
    CHECK(seed_session->wait_for_upload_complete_or_client_stopped());

    std::mutex error_mutex;
    bool got_compensating_write = false;

    Session::Config config_a;
    config_a.signed_user_token = g_user_0_path_test_token;
    config_a.user_id = "user_0";
    config_a.connection_state_change_listener = [&](ConnectionState state,
                                                    std::optional<SessionErrorInfo> error_info) {
        if (error_info && error_info->status == ErrorCodes::SyncCompensatingWrite) {
            std::lock_guard<std::mutex> lock(error_mutex);
            CHECK_EQUAL(state, ConnectionState::connected);
            CHECK_EQUAL(error_info->compensating_writes.size(), 1);
            got_compensating_write = true;
            return;
        }
        if (state == ConnectionState::disconnected && error_info) {
            test_context.logger->error("Client disconnect: %1 (is_fatal=%2)", error_info->status,
                                       error_info->is_fatal);
            bool client_error_occurred = true;
            CHECK_NOT(client_error_occurred);
            fixture.stop();
        }
    };

    util::Optional<Session> session_a =
        fixture.make_flx_session(1, 0, db_a, sub_store_a, "/test", std::move(config_a));
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());
    check_private_count(test_context, db_a, 0);

    upsert_flx_private_object(db_a, 42, "not allowed");
    CHECK(session_a->wait_for_upload_complete_or_client_stopped());
    CHECK(session_a->wait_for_download_complete_or_client_stopped());

    {
        std::lock_guard<std::mutex> lock(error_mutex);
        CHECK(got_compensating_write);
    }
    check_private_count(test_context, db_a, 0);
}

TEST(Sync_TransformAgainstEmptyReciprocalChangeset)
{
    TEST_CLIENT_DB(seed_db);
    TEST_CLIENT_DB(db_1);
    TEST_CLIENT_DB(db_2);

    {
        auto tr = seed_db->start_write();
        // Create schema: single table with array of ints as property.
        auto table = tr->add_table_with_primary_key("class_table", type_Int, "_id");
        table->add_column_list(type_Int, "ints");
        table->add_column(type_String, "string");
        tr->commit_and_continue_writing();

        // Create object and initialize array.
        table = tr->get_table("class_table");
        auto obj = table->create_object_with_primary_key(42);
        auto ints = obj.get_list<int64_t>("ints");
        for (auto i = 0; i < 8; ++i) {
            ints.insert(i, i);
        }
        tr->commit();
    }

    {
        TEST_DIR(dir);
        MultiClientServerFixture fixture(3, 1, dir, test_context);
        fixture.start();

        util::Optional<Session> seed_session = fixture.make_bound_session(0, seed_db, 0, "/test");
        util::Optional<Session> db_1_session = fixture.make_bound_session(1, db_1, 0, "/test");
        util::Optional<Session> db_2_session = fixture.make_bound_session(2, db_2, 0, "/test");

        seed_session->wait_for_upload_complete_or_client_stopped();
        db_1_session->wait_for_download_complete_or_client_stopped();
        db_2_session->wait_for_download_complete_or_client_stopped();
        seed_session.reset();
        db_2_session.reset();

        auto move_element = [&](const DBRef& db, size_t from, size_t to, size_t string_size = 0) {
            auto wt = db->start_write();
            auto table = wt->get_table("class_table");
            auto obj = table->get_object_with_primary_key(42);
            auto ints = obj.get_list<int64_t>("ints");
            ints.move(from, to);
            obj.set("string", std::string(string_size, 'a'));
            wt->commit();
        };

        // Client 1 uploads two move instructions.
        move_element(db_1, 7, 2);
        move_element(db_1, 7, 6);

        db_1_session->wait_for_upload_complete_or_client_stopped();

        std::this_thread::sleep_for(std::chrono::milliseconds{10});

        // Client 2 uploads two move instructions.
        // The sync client uploads at most 128 KB of data so we make the first changeset large enough so two upload
        // messages are sent to the server instead of one. Each change is transformed against the changes from
        // Client 1.

        // First change discards the first change (move(7, 2)) of Client 1.
        move_element(db_2, 7, 0, 200 * 1024);
        // Second change is tranformed against an empty reciprocal changeset as result of the change above.
        move_element(db_2, 7, 5);
        db_2_session = fixture.make_bound_session(2, db_2, 0, "/test");

        db_1_session->wait_for_upload_complete_or_client_stopped();
        db_2_session->wait_for_upload_complete_or_client_stopped();

        db_1_session->wait_for_download_complete_or_client_stopped();
        db_2_session->wait_for_download_complete_or_client_stopped();
    }

    ReadTransaction rt_1(db_1);
    ReadTransaction rt_2(db_2);
    const Group& group_1 = rt_1;
    const Group& group_2 = rt_2;
    group_1.verify();
    group_2.verify();
    CHECK(compare_groups(rt_1, rt_2));
}

#endif // !BARQ_MOBILE

// Tests that an empty reciprocal changesets is set and retrieved correctly.
TEST(Sync_SetAndGetEmptyReciprocalChangeset)
{
    using namespace barq;
    using namespace barq::sync::instr;
    using barq::sync::Changeset;

    TEST_CLIENT_DB(db);

    auto& history = get_history(db);
    history.set_client_file_ident(SaltedFileIdent{1, 0x1234567812345678}, false);
    timestamp_type timestamp{1};
    history.set_local_origin_timestamp_source([&] {
        return ++timestamp;
    });

    auto latest_local_version = [&] {
        auto tr = db->start_write();
        // Create schema: single table with array of ints as property.
        tr->add_table_with_primary_key("class_table", type_Int, "_id")->add_column_list(type_Int, "ints");
        tr->commit_and_continue_writing();

        // Create object and initialize array.
        TableRef table = tr->get_table("class_table");
        auto obj = table->create_object_with_primary_key(42);
        auto ints = obj.get_list<int64_t>("ints");
        for (auto i = 0; i < 8; ++i) {
            ints.insert(i, i);
        }
        tr->commit_and_continue_writing();

        // Move element in array.
        ints.move(7, 2);
        return tr->commit();
    }();

    // Create changeset which moves element from index 7 to index 0 in array.
    // This changeset will discard the previous move (reciprocal changeset),
    // leaving the local reciprocal changeset with no instructions (empty).
    Changeset changeset;
    ArrayMove instr;
    instr.table = changeset.intern_string("table");
    instr.object = instr::PrimaryKey{42};
    instr.field = changeset.intern_string("ints");
    instr.path.push_back(7);
    instr.ndx_2 = 0;
    instr.prior_size = 8;
    changeset.push_back(instr);
    changeset.version = 1;
    changeset.last_integrated_remote_version = latest_local_version - 1;
    changeset.origin_timestamp = timestamp;
    changeset.origin_file_ident = 2;

    ChangesetEncoder::Buffer encoded;
    std::vector<RemoteChangeset> server_changesets_encoded;
    encode_changeset(changeset, encoded);
    server_changesets_encoded.emplace_back(changeset.version, changeset.last_integrated_remote_version,
                                           BinaryData(encoded.data(), encoded.size()), changeset.origin_timestamp,
                                           changeset.origin_file_ident);

    SyncProgress progress = {};
    progress.download.server_version = changeset.version;
    progress.download.last_integrated_client_version = changeset.last_integrated_remote_version;
    progress.latest_server_version.version = changeset.version;
    progress.latest_server_version.salt = 0x7876543217654321;

    uint_fast64_t downloadable_bytes = 0;
    VersionInfo version_info;
    auto transact = db->start_read();
    history.integrate_server_changesets(progress, downloadable_bytes, server_changesets_encoded, version_info,
                                        DownloadBatchState::SteadyState, *test_context.logger, transact);

    auto reciprocal_changeset = get_reciprocal_changeset(history, latest_local_version);
    // The only instruction in the reciprocal changeset was discarded during OT.
    CHECK(reciprocal_changeset.empty());
}

TEST(Sync_SetEmptyReciprocalChangesetAfterNonEmptyReciprocalChangeset)
{
    using namespace barq;
    using namespace barq::sync::instr;
    using barq::sync::Changeset;

    TEST_CLIENT_DB(db);

    auto& history = get_history(db);
    history.set_client_file_ident(SaltedFileIdent{1, 0x1234567812345678}, false);
    timestamp_type timestamp{1};
    history.set_local_origin_timestamp_source([&] {
        return ++timestamp;
    });

    auto latest_local_version = [&] {
        auto tr = db->start_write();
        // Create schema: single table with array of ints as property.
        tr->add_table_with_primary_key("class_table", type_Int, "_id")->add_column_list(type_Int, "ints");
        tr->commit_and_continue_writing();

        // Create object and initialize array.
        TableRef table = tr->get_table("class_table");
        auto obj = table->create_object_with_primary_key(42);
        auto ints = obj.get_list<int64_t>("ints");
        ints.insert(0, 1);
        ints.insert(1, 2);
        ints.insert(2, 3);
        tr->commit_and_continue_writing();

        // Update two elements in the list.

        ints.set_any(2, Mixed{4});
        tr->commit_and_continue_writing();

        ints.set_any(0, Mixed{5});
        return tr->commit();
    }();

    // Create remote changeset (erase at the same index) that:
    //  1. Updates the prior_size of the first local update
    //  2. Discards the second local update
    // After OT, we end up with two reciprocal changesets: a non-empty
    // and an empty one.
    Changeset changeset;
    ArrayErase instr;
    instr.table = changeset.intern_string("table");
    instr.object = instr::PrimaryKey{42};
    instr.field = changeset.intern_string("ints");
    instr.prior_size = 3;
    instr.path.push_back(0);
    changeset.push_back(instr);
    changeset.version = 1;
    // Make it so the merging window contains the last two local updates.
    changeset.last_integrated_remote_version = latest_local_version - 2;
    changeset.origin_timestamp = timestamp;
    changeset.origin_file_ident = 2;

    ChangesetEncoder::Buffer encoded;
    std::vector<RemoteChangeset> server_changesets_encoded;
    encode_changeset(changeset, encoded);
    server_changesets_encoded.emplace_back(changeset.version, changeset.last_integrated_remote_version,
                                           BinaryData(encoded.data(), encoded.size()), changeset.origin_timestamp,
                                           changeset.origin_file_ident);

    SyncProgress progress = {};
    progress.download.server_version = changeset.version;
    progress.download.last_integrated_client_version = changeset.last_integrated_remote_version;
    progress.latest_server_version.version = changeset.version;
    progress.latest_server_version.salt = 0x7876543217654321;

    uint_fast64_t downloadable_bytes = 0;
    VersionInfo version_info;
    auto transact = db->start_read();
    history.integrate_server_changesets(progress, downloadable_bytes, server_changesets_encoded, version_info,
                                        DownloadBatchState::SteadyState, *test_context.logger, transact);

    // The first reciprocal changeset has the prior_size changed.
    auto reciprocal_changeset = get_reciprocal_changeset(history, latest_local_version - 1);
    CHECK_EQUAL(reciprocal_changeset.size(), 1);
    auto instruction = reciprocal_changeset.begin()->get_if<Instruction::Update>();
    CHECK_EQUAL(instruction->prior_size, 2);
    CHECK_EQUAL(instruction->value.data.integer, 4);

    reciprocal_changeset = get_reciprocal_changeset(history, latest_local_version);
    // The second reciprocal changeset is empty.
    CHECK(reciprocal_changeset.empty());
}

TEST(Sync_GetEmptyReciprocalChangesetFromCache)
{
    using namespace barq;
    using namespace barq::sync::instr;
    using barq::sync::Changeset;

    TEST_CLIENT_DB(db);

    auto& history = get_history(db);
    history.set_client_file_ident(SaltedFileIdent{1, 0x1234567812345678}, false);
    timestamp_type timestamp{1};
    history.set_local_origin_timestamp_source([&] {
        return ++timestamp;
    });

    auto latest_local_version = [&] {
        auto tr = db->start_write();
        // Create schema
        TableRef table = tr->add_table_with_primary_key("class_table", type_Int, "_id");
        table->add_column_list(type_Int, "ints");
        table->add_column_dictionary(type_Int, "dict");
        tr->commit_and_continue_writing();

        // Create object
        auto obj = table->create_object_with_primary_key(42);
        auto ints = obj.get_list<int64_t>("ints");
        ints.insert(0, 1);
        ints.insert(1, 2);
        ints.insert(2, 3);
        tr->commit_and_continue_writing();

        // Update list
        ints.set_any(2, Mixed{4});
        tr->commit_and_continue_writing();

        // Update dictionary
        auto dict = obj.get_dictionary("dict");
        dict.insert("key", 42);
        return tr->commit();
    }();

    std::vector<Changeset> server_changesets;
    // Create remote changeset that updates the list and discards
    // the (local) dictionary update.
    Changeset changeset;
    ArrayErase instr;
    instr.table = changeset.intern_string("table");
    instr.object = instr::PrimaryKey{42};
    instr.field = changeset.intern_string("ints");
    instr.prior_size = 3;
    instr.path.push_back(0);
    changeset.push_back(instr);
    Update instr2;
    instr2.table = changeset.intern_string("table");
    instr2.object = instr::PrimaryKey{42};
    instr2.field = changeset.intern_string("dict");
    auto key = changeset.intern_string("key2");
    instr2.path.push_back(key);
    instr2.value = Payload{int64_t(0)};
    changeset.push_back(instr2);
    Clear instr3;
    instr3.table = changeset.intern_string("table");
    instr3.object = instr::PrimaryKey{42};
    instr3.field = changeset.intern_string("dict");
    instr3.collection_type = instr::CollectionType::Dictionary;
    changeset.push_back(instr3);
    changeset.version = 1;
    // Make it so the merging window contains the last two local updates.
    changeset.last_integrated_remote_version = latest_local_version - 2;
    changeset.origin_timestamp = timestamp - 2;
    changeset.origin_file_ident = 2;
    server_changesets.push_back(changeset);

    // Create changeset that inserts the same key as the local (discarded) update.
    Changeset changeset2;
    Update instr4;
    instr4.table = changeset2.intern_string("table");
    instr4.object = instr::PrimaryKey{42};
    instr4.field = changeset2.intern_string("dict");
    key = changeset2.intern_string("key");
    instr4.path.push_back(key);
    instr4.value = Payload{int64_t(-6)};
    changeset2.push_back(instr4);
    changeset2.version = 2;
    // Make it so the merging window contains the local dictionary update.
    changeset2.last_integrated_remote_version = latest_local_version - 1;
    changeset2.origin_timestamp = timestamp - 1;
    changeset2.origin_file_ident = 2;
    server_changesets.push_back(changeset2);

    std::vector<ChangesetEncoder::Buffer> encoded;
    std::vector<RemoteChangeset> server_changesets_encoded;
    for (const auto& changeset : server_changesets) {
        encoded.emplace_back();
        encode_changeset(changeset, encoded.back());
        server_changesets_encoded.emplace_back(changeset.version, changeset.last_integrated_remote_version,
                                               BinaryData(encoded.back().data(), encoded.back().size()),
                                               changeset.origin_timestamp, changeset.origin_file_ident);
    }

    SyncProgress progress = {};
    progress.download.server_version = server_changesets.back().version;
    // Prevent history being trimmed when server changes are integrated so we can verify the reciprocal changesets.
    progress.download.last_integrated_client_version = server_changesets.front().last_integrated_remote_version;
    progress.latest_server_version.version = server_changesets.back().version;
    progress.latest_server_version.salt = 0x7876543217654321;

    uint_fast64_t downloadable_bytes = 0;
    VersionInfo version_info;
    auto transact = db->start_read();
    history.integrate_server_changesets(progress, downloadable_bytes, server_changesets_encoded, version_info,
                                        DownloadBatchState::SteadyState, *test_context.logger, transact);

    // The remote dictionary update persists.
    auto tr = db->start_read();
    auto dict = tr->get_table("class_table")->get_object_with_primary_key(42).get_dictionary("dict");
    CHECK(!dict.is_empty());
    CHECK(dict.get("key") == -6);

    // The first reciprocal changeset has the prior_size changed.
    auto reciprocal_changeset = get_reciprocal_changeset(history, latest_local_version - 1);
    CHECK_EQUAL(reciprocal_changeset.size(), 1);
    auto instruction = reciprocal_changeset.begin()->get_if<Instruction::Update>();
    CHECK_EQUAL(instruction->prior_size, 2);
    CHECK_EQUAL(instruction->value.data.integer, 4);
    // The second reciprocal changeset is empty.
    reciprocal_changeset = get_reciprocal_changeset(history, latest_local_version);
    CHECK(reciprocal_changeset.empty());
}

TEST(Sync_GetEmptyReciprocalChangesetFromArray)
{
    using namespace barq;
    using namespace barq::sync::instr;
    using barq::sync::Changeset;

    TEST_CLIENT_DB(db);

    auto& history = get_history(db);
    history.set_client_file_ident(SaltedFileIdent{1, 0x1234567812345678}, false);
    timestamp_type timestamp{1};
    history.set_local_origin_timestamp_source([&] {
        return ++timestamp;
    });

    auto latest_local_version = [&] {
        auto tr = db->start_write();
        // Create schema
        TableRef table = tr->add_table_with_primary_key("class_table", type_Int, "_id");
        table->add_column_list(type_Int, "ints");
        table->add_column_dictionary(type_Int, "dict");
        tr->commit_and_continue_writing();

        // Create object
        auto obj = table->create_object_with_primary_key(42);
        auto ints = obj.get_list<int64_t>("ints");
        ints.insert(0, 1);
        ints.insert(1, 2);
        ints.insert(2, 3);
        tr->commit_and_continue_writing();

        // Update list
        ints.set_any(2, Mixed{4});
        tr->commit_and_continue_writing();

        // Update dictionary
        auto dict = obj.get_dictionary("dict");
        dict.insert("key", 42);
        return tr->commit();
    }();

    std::vector<Changeset> server_changesets;
    // Create remote changeset that updates the list and discards
    // the (local) dictionary update.
    Changeset changeset;
    ArrayErase instr;
    instr.table = changeset.intern_string("table");
    instr.object = instr::PrimaryKey{42};
    instr.field = changeset.intern_string("ints");
    instr.prior_size = 3;
    instr.path.push_back(0);
    changeset.push_back(instr);
    Update instr2;
    instr2.table = changeset.intern_string("table");
    instr2.object = instr::PrimaryKey{42};
    instr2.field = changeset.intern_string("dict");
    auto key = changeset.intern_string("key2");
    instr2.path.push_back(key);
    instr2.value = Payload{int64_t(0)};
    changeset.push_back(instr2);
    Clear instr3;
    instr3.table = changeset.intern_string("table");
    instr3.object = instr::PrimaryKey{42};
    instr3.field = changeset.intern_string("dict");
    instr3.collection_type = instr::CollectionType::Dictionary;
    changeset.push_back(instr3);
    changeset.version = 1;
    // Make it so the merging window contains the last two local updates.
    changeset.last_integrated_remote_version = latest_local_version - 2;
    changeset.origin_timestamp = timestamp - 2;
    changeset.origin_file_ident = 2;
    server_changesets.push_back(changeset);

    // Create changeset that inserts the same key as the local (discarded) update.
    Changeset changeset2;
    Update instr4;
    instr4.table = changeset2.intern_string("table");
    instr4.object = instr::PrimaryKey{42};
    instr4.field = changeset2.intern_string("dict");
    key = changeset2.intern_string("key");
    instr4.path.push_back(key);
    instr4.value = Payload{int64_t(-6)};
    changeset2.push_back(instr4);
    changeset2.version = 2;
    // Make it so the merging window contains the local dictionary update.
    changeset2.last_integrated_remote_version = latest_local_version - 1;
    changeset2.origin_timestamp = timestamp - 1;
    changeset2.origin_file_ident = 2;
    server_changesets.push_back(changeset2);

    std::vector<RemoteChangeset> server_changesets_encoded;
    for (const auto& changeset : server_changesets) {
        ChangesetEncoder::Buffer encoded;
        encode_changeset(changeset, encoded);
        server_changesets_encoded.clear();
        server_changesets_encoded.emplace_back(changeset.version, changeset.last_integrated_remote_version,
                                               BinaryData(encoded.data(), encoded.size()), changeset.origin_timestamp,
                                               changeset.origin_file_ident);

        SyncProgress progress = {};
        progress.download.server_version = changeset.version;
        // Prevent history being trimmed when server changes are integrated so we can verify the reciprocal
        // changesets.
        progress.download.last_integrated_client_version = server_changesets.front().last_integrated_remote_version;
        progress.latest_server_version.version = changeset.version;
        progress.latest_server_version.salt = 0x7876543217654321;

        uint_fast64_t downloadable_bytes = 0;
        VersionInfo version_info;
        auto transact = db->start_read();
        history.integrate_server_changesets(progress, downloadable_bytes, server_changesets_encoded, version_info,
                                            DownloadBatchState::SteadyState, *test_context.logger, transact);
    }

    // The remote dictionary update persists.
    auto tr = db->start_read();
    auto dict = tr->get_table("class_table")->get_object_with_primary_key(42).get_dictionary("dict");
    CHECK(!dict.is_empty());
    CHECK(dict.get("key") == -6);

    // The first reciprocal changeset has the prior_size changed.
    auto reciprocal_changeset = get_reciprocal_changeset(history, latest_local_version - 1);
    CHECK_EQUAL(reciprocal_changeset.size(), 1);
    auto instruction = reciprocal_changeset.begin()->get_if<Instruction::Update>();
    CHECK_EQUAL(instruction->prior_size, 2);
    CHECK_EQUAL(instruction->value.data.integer, 4);
    // The second reciprocal changeset is empty.
    reciprocal_changeset = get_reciprocal_changeset(history, latest_local_version);
    CHECK(reciprocal_changeset.empty());
}

TEST(Sync_InvalidChangesetFromServer)
{
    TEST_CLIENT_DB(db);

    auto& history = get_history(db);
    history.set_client_file_ident(SaltedFileIdent{2, 0x1234567812345678}, false);

    instr::CreateObject bad_instr;
    bad_instr.object = InternString{1};
    bad_instr.table = InternString{2};

    Changeset changeset;
    changeset.push_back(bad_instr);

    ChangesetEncoder::Buffer encoded;
    encode_changeset(changeset, encoded);
    RemoteChangeset server_changeset;
    server_changeset.origin_file_ident = 1;
    server_changeset.remote_version = 1;
    server_changeset.data = BinaryData(encoded.data(), encoded.size());

    VersionInfo version_info;
    auto transact = db->start_read();
    CHECK_THROW_EX(history.integrate_server_changesets({}, 0, util::Span(&server_changeset, 1), version_info,
                                                       DownloadBatchState::SteadyState, *test_context.logger,
                                                       transact),
                   sync::IntegrationException,
                   StringData(e.what()).contains("Failed to parse received changeset: Invalid interned string"));
}

TEST(Sync_ServerVersionsSkippedFromDownloadCursor)
{
    TEST_CLIENT_DB(db);

    auto& history = get_history(db);
    history.set_client_file_ident(SaltedFileIdent{2, 0x1234567812345678}, false);
    timestamp_type timestamp{1};
    history.set_local_origin_timestamp_source([&] {
        return ++timestamp;
    });

    auto latest_local_version = [&] {
        auto tr = db->start_write();
        tr->add_table_with_primary_key("class_foo", type_String, "_id")->add_column(type_Int, "int_col");
        return tr->commit();
    }();

    Changeset server_changeset;
    server_changeset.version = 10;
    server_changeset.last_integrated_remote_version = latest_local_version - 1;
    server_changeset.origin_timestamp = ++timestamp;
    server_changeset.origin_file_ident = 1;

    std::vector<ChangesetEncoder::Buffer> encoded;
    std::vector<RemoteChangeset> server_changesets_encoded;
    encoded.emplace_back();
    encode_changeset(server_changeset, encoded.back());
    server_changesets_encoded.emplace_back(server_changeset.version, server_changeset.last_integrated_remote_version,
                                           BinaryData(encoded.back().data(), encoded.back().size()),
                                           server_changeset.origin_timestamp, server_changeset.origin_file_ident);

    SyncProgress progress = {};
    // The server skips 10 server versions.
    progress.download.server_version = server_changeset.version + 10;
    progress.download.last_integrated_client_version = latest_local_version - 1;
    progress.latest_server_version.version = server_changeset.version + 15;
    progress.latest_server_version.salt = 0x7876543217654321;

    uint_fast64_t downloadable_bytes = 0;
    VersionInfo version_info;
    auto transact = db->start_read();
    history.integrate_server_changesets(progress, downloadable_bytes, server_changesets_encoded, version_info,
                                        DownloadBatchState::SteadyState, *test_context.logger, transact);

    version_type current_version;
    SaltedFileIdent file_ident;
    SyncProgress expected_progress;
    history.get_status(current_version, file_ident, expected_progress);

    // Check progress is reported correctly.
    CHECK_EQUAL(progress.latest_server_version.salt, expected_progress.latest_server_version.salt);
    CHECK_EQUAL(progress.latest_server_version.version, expected_progress.latest_server_version.version);
    CHECK_EQUAL(progress.download.last_integrated_client_version,
                expected_progress.download.last_integrated_client_version);
    CHECK_EQUAL(progress.download.server_version, expected_progress.download.server_version);
    CHECK_EQUAL(progress.upload.client_version, expected_progress.upload.client_version);
    CHECK_EQUAL(progress.upload.last_integrated_server_version,
                expected_progress.upload.last_integrated_server_version);
}

TEST(Sync_NonIncreasingServerVersions)
{
    TEST_CLIENT_DB(db);

    auto& history = get_history(db);
    history.set_client_file_ident(SaltedFileIdent{2, 0x1234567812345678}, false);
    timestamp_type timestamp{1};
    history.set_local_origin_timestamp_source([&] {
        return ++timestamp;
    });

    auto latest_local_version = [&] {
        auto tr = db->start_write();
        tr->add_table_with_primary_key("class_foo", type_String, "_id")->add_column(type_Int, "int_col");
        return tr->commit();
    }();

    std::vector<Changeset> server_changesets;
    auto prep_changeset = [&](auto pk_name, auto int_col_val) {
        Changeset changeset;
        changeset.version = 10;
        changeset.last_integrated_remote_version = latest_local_version - 1;
        changeset.origin_timestamp = ++timestamp;
        changeset.origin_file_ident = 1;
        instr::PrimaryKey pk{changeset.intern_string(pk_name)};
        auto table_name = changeset.intern_string("foo");
        auto col_name = changeset.intern_string("int_col");
        instr::EraseObject erase_1;
        erase_1.object = pk;
        erase_1.table = table_name;
        changeset.push_back(erase_1);
        instr::CreateObject create_1;
        create_1.object = pk;
        create_1.table = table_name;
        changeset.push_back(create_1);
        instr::Update update_1;
        update_1.table = table_name;
        update_1.object = pk;
        update_1.field = col_name;
        update_1.value = instr::Payload{int64_t(int_col_val)};
        changeset.push_back(update_1);
        server_changesets.push_back(std::move(changeset));
    };
    prep_changeset("bizz", 1);
    prep_changeset("buzz", 2);
    prep_changeset("baz", 3);
    prep_changeset("bar", 4);
    ++server_changesets.back().version;

    std::vector<ChangesetEncoder::Buffer> encoded;
    std::vector<RemoteChangeset> server_changesets_encoded;
    for (const auto& changeset : server_changesets) {
        encoded.emplace_back();
        encode_changeset(changeset, encoded.back());
        server_changesets_encoded.emplace_back(changeset.version, changeset.last_integrated_remote_version,
                                               BinaryData(encoded.back().data(), encoded.back().size()),
                                               changeset.origin_timestamp, changeset.origin_file_ident);
    }

    SyncProgress progress = {};
    progress.download.server_version = server_changesets.back().version;
    progress.download.last_integrated_client_version = latest_local_version - 1;
    progress.latest_server_version.version = server_changesets.back().version;
    progress.latest_server_version.salt = 0x7876543217654321;

    uint_fast64_t downloadable_bytes = 0;
    VersionInfo version_info;
    auto transact = db->start_read();
    history.integrate_server_changesets(progress, downloadable_bytes, server_changesets_encoded, version_info,
                                        DownloadBatchState::SteadyState, *test_context.logger, transact);
}

TEST(Sync_DanglingLinksCountInPriorSize)
{
    SHARED_GROUP_TEST_PATH(path);
    ClientReplication repl;
    auto local_db = barq::DB::create(repl, path);
    auto& history = repl.get_history();
    history.set_client_file_ident(sync::SaltedFileIdent{1, 123456}, true);

    version_type last_version, last_version_observed = 0;
    auto dump_uploadable = [&] {
        UploadCursor upload_cursor{last_version_observed, 0};
        std::vector<sync::ClientHistory::UploadChangeset> changesets_to_upload;
        version_type locked_server_version = 0;
        history.find_uploadable_changesets(upload_cursor, last_version, changesets_to_upload, locked_server_version);
        CHECK_EQUAL(changesets_to_upload.size(), static_cast<size_t>(1));
        barq::sync::Changeset parsed_changeset;
        auto unparsed_changeset = changesets_to_upload[0].changeset.get_first_chunk();
        barq::util::SimpleInputStream changeset_stream(unparsed_changeset);
        barq::sync::parse_changeset(changeset_stream, parsed_changeset);
        test_context.logger->info("changeset at version %1: %2", last_version, parsed_changeset);
        last_version_observed = last_version;
        return parsed_changeset;
    };

    TableKey source_table_key, target_table_key;
    {
        auto wt = local_db->start_write();
        auto source_table = wt->add_table_with_primary_key("class_source", type_String, "_id");
        auto target_table = wt->add_table_with_primary_key("class_target", type_String, "_id");
        source_table->add_column_list(*target_table, "links");

        source_table_key = source_table->get_key();
        target_table_key = target_table->get_key();

        auto obj_to_keep = target_table->create_object_with_primary_key(std::string{"target1"});
        auto obj_to_delete = target_table->create_object_with_primary_key(std::string{"target2"});
        auto source_obj = source_table->create_object_with_primary_key(std::string{"source"});

        auto links_list = source_obj.get_linklist("links");
        links_list.add(obj_to_keep.get_key());
        links_list.add(obj_to_delete.get_key());
        last_version = wt->commit();
    }

    dump_uploadable();

    {
        // Simulate removing the object via the sync client so we get a dangling link
        TempShortCircuitReplication disable_repl(repl);
        auto wt = local_db->start_write();
        auto target_table = wt->get_table(target_table_key);
        auto obj = target_table->get_object_with_primary_key(std::string{"target2"});
        obj.invalidate();
        last_version = wt->commit();
    }

    {
        auto wt = local_db->start_write();
        auto source_table = wt->get_table(source_table_key);
        auto target_table = wt->get_table(target_table_key);

        auto obj_to_add = target_table->create_object_with_primary_key(std::string{"target3"});

        auto source_obj = source_table->get_object_with_primary_key(std::string{"source"});
        auto links_list = source_obj.get_linklist("links");
        links_list.add(obj_to_add.get_key());
        last_version = wt->commit();
    }

    auto changeset = dump_uploadable();
    CHECK_EQUAL(changeset.size(), static_cast<size_t>(2));
    auto changeset_it = changeset.end();
    --changeset_it;
    auto last_instr = *changeset_it;
    CHECK_EQUAL(last_instr->type(), Instruction::Type::ArrayInsert);
    auto arr_insert_instr = last_instr->get_as<Instruction::ArrayInsert>();
    CHECK_EQUAL(changeset.get_string(arr_insert_instr.table), StringData("source"));
    CHECK(arr_insert_instr.value.type == sync::instr::Payload::Type::Link);
    CHECK_EQUAL(changeset.get_string(mpark::get<InternString>(arr_insert_instr.value.data.link.target)),
                StringData("target3"));
    CHECK_EQUAL(arr_insert_instr.prior_size, 2);
}

// This test calls row_for_object_id() for various object ids and tests that
// the right value is returned including that no assertions are hit.
TEST(Sync_RowForGlobalKey)
{
    TEST_CLIENT_DB(db);

    {
        WriteTransaction wt(db);
        TableRef table = wt.add_table("class_foo");
        table->add_column(type_Int, "i");
        wt.commit();
    }

    // Check that various object_ids are not in the table.
    {
        ReadTransaction rt(db);
        ConstTableRef table = rt.get_table("class_foo");
        CHECK(table);

        // Default constructed GlobalKey
        {
            GlobalKey object_id;
            auto row_ndx = table->get_objkey(object_id);
            CHECK_NOT(row_ndx);
        }

        // GlobalKey with small lo and hi values
        {
            GlobalKey object_id{12, 24};
            auto row_ndx = table->get_objkey(object_id);
            CHECK_NOT(row_ndx);
        }

        // GlobalKey with lo and hi values past the 32 bit limit.
        {
            GlobalKey object_id{uint_fast64_t(1) << 50, uint_fast64_t(1) << 52};
            auto row_ndx = table->get_objkey(object_id);
            CHECK_NOT(row_ndx);
        }
    }
}

TEST(Sync_FirstPromoteToWriteAdvancesRead)
{
    TEST_CLIENT_DB(db);
    auto db2 = DB::create(make_client_replication(), db_path);
    auto read = db->start_read();
    db2->start_write()->commit();
    // This will hit `ClientHistory::update_from_ref_and_version()` with m_group
    // unset since it's advancing the read transaction without ever having been
    // in a write transaction before.
    read->promote_to_write();
}


} // unnamed namespace
