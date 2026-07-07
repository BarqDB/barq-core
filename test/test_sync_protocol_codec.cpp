/*************************************************************************
 *
 * Copyright 2023 Realm Inc.
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

#include "testsettings.hpp"
#include "test.hpp"

#include <barq/sync/noinst/protocol_codec.hpp>

#include <string>
#include <memory>

using namespace barq;
using namespace barq::util;

using TestContext = test_util::unit_test::TestContext;

template <class A, class B>
void compare_out_string(const A& expected, const B& out, TestContext& test_context)
{
    CHECK_EQUAL(std::string_view(expected.data(), expected.size()), std::string_view(out.data(), out.size()));
}

struct ClientProtocolTestConnection {
    util::Logger& logger;
    bool flx_sync = true;
    bool got_download = false;
    Status protocol_error = Status::OK();
    _impl::ClientProtocol::session_ident_type download_session_ident = 0;
    _impl::ClientProtocol::DownloadMessage download_message;

    bool is_flx_sync_connection() const
    {
        return flx_sync;
    }

    void handle_protocol_error(Status status)
    {
        protocol_error = std::move(status);
    }

    void receive_download_message(_impl::ClientProtocol::session_ident_type session_ident,
                                  _impl::ClientProtocol::DownloadMessage message)
    {
        got_download = true;
        download_session_ident = session_ident;
        download_message = std::move(message);
    }

    void receive_pong(_impl::ClientProtocol::milliseconds_type) {}
    void receive_unbound_message(_impl::ClientProtocol::session_ident_type) {}
    void receive_error_message(sync::ProtocolErrorInfo, _impl::ClientProtocol::session_ident_type) {}
    void receive_query_error_message(int, std::string_view, int64_t, _impl::ClientProtocol::session_ident_type) {}
    void receive_mark_message(_impl::ClientProtocol::session_ident_type, _impl::ClientProtocol::request_ident_type) {}
    void receive_ident_message(_impl::ClientProtocol::session_ident_type, _impl::ClientProtocol::SaltedFileIdent) {}
    void receive_test_command_response(_impl::ClientProtocol::session_ident_type,
                                       _impl::ClientProtocol::request_ident_type, std::string_view)
    {
    }
    void receive_barq_request_id(std::string_view) {}
    void receive_server_log_message(_impl::ClientProtocol::session_ident_type, util::Logger::Level, std::string_view)
    {
    }
};

struct ServerProtocolTestConnection {
    util::Logger& logger;
    bool flx_sync = true;
    Status protocol_error = Status::OK();

    bool got_bind = false;
    _impl::ServerProtocol::session_ident_type bind_session_ident = 0;
    std::string bind_path;
    std::string bind_signed_user_token;
    bool bind_need_client_file_ident = false;
    bool bind_is_subserver = false;

    bool got_flx_ident = false;
    _impl::ServerProtocol::session_ident_type ident_session_ident = 0;
    _impl::ServerProtocol::file_ident_type ident_client_file_ident = 0;
    _impl::ServerProtocol::salt_type ident_client_file_ident_salt = 0;
    _impl::ServerProtocol::version_type ident_scan_server_version = 0;
    _impl::ServerProtocol::version_type ident_scan_client_version = 0;
    _impl::ServerProtocol::version_type ident_latest_server_version = 0;
    _impl::ServerProtocol::salt_type ident_latest_server_version_salt = 0;
    int64_t ident_query_version = 0;
    std::string ident_query_body;

    bool got_query = false;
    _impl::ServerProtocol::session_ident_type query_session_ident = 0;
    int64_t query_version = 0;
    std::string query_body;

    bool is_flx_sync_connection() const
    {
        return flx_sync;
    }

    void handle_protocol_error(Status status)
    {
        protocol_error = std::move(status);
    }

    void receive_upload_message(_impl::ServerProtocol::session_ident_type, _impl::ServerProtocol::version_type,
                                _impl::ServerProtocol::version_type, _impl::ServerProtocol::version_type,
                                const std::vector<_impl::ServerProtocol::UploadChangeset>&)
    {
    }
    void receive_mark_message(_impl::ServerProtocol::session_ident_type, _impl::ServerProtocol::request_ident_type) {}
    void receive_ping(_impl::ServerProtocol::milliseconds_type, _impl::ServerProtocol::milliseconds_type) {}
    void receive_bind_message(_impl::ServerProtocol::session_ident_type session_ident, std::string path,
                              std::string signed_user_token, bool need_client_file_ident, bool is_subserver)
    {
        got_bind = true;
        bind_session_ident = session_ident;
        bind_path = std::move(path);
        bind_signed_user_token = std::move(signed_user_token);
        bind_need_client_file_ident = need_client_file_ident;
        bind_is_subserver = is_subserver;
    }
    void receive_ident_message(_impl::ServerProtocol::session_ident_type, _impl::ServerProtocol::file_ident_type,
                               _impl::ServerProtocol::salt_type, _impl::ServerProtocol::version_type,
                               _impl::ServerProtocol::version_type, _impl::ServerProtocol::version_type,
                               _impl::ServerProtocol::salt_type)
    {
    }
    void receive_ident_message(_impl::ServerProtocol::session_ident_type session_ident,
                               _impl::ServerProtocol::file_ident_type client_file_ident,
                               _impl::ServerProtocol::salt_type client_file_ident_salt,
                               _impl::ServerProtocol::version_type scan_server_version,
                               _impl::ServerProtocol::version_type scan_client_version,
                               _impl::ServerProtocol::version_type latest_server_version,
                               _impl::ServerProtocol::salt_type latest_server_version_salt, int64_t query_version,
                               std::string query_body)
    {
        got_flx_ident = true;
        ident_session_ident = session_ident;
        ident_client_file_ident = client_file_ident;
        ident_client_file_ident_salt = client_file_ident_salt;
        ident_scan_server_version = scan_server_version;
        ident_scan_client_version = scan_client_version;
        ident_latest_server_version = latest_server_version;
        ident_latest_server_version_salt = latest_server_version_salt;
        ident_query_version = query_version;
        ident_query_body = std::move(query_body);
    }
    void receive_query_message(_impl::ServerProtocol::session_ident_type session_ident, int64_t version,
                               std::string body)
    {
        got_query = true;
        query_session_ident = session_ident;
        query_version = version;
        query_body = std::move(body);
    }
    void receive_unbind_message(_impl::ServerProtocol::session_ident_type) {}
    void receive_error_message(_impl::ServerProtocol::session_ident_type, int, std::string_view) {}
};

TEST(Protocol_Codec_Bind_PBS)
{
    auto protocol = _impl::ClientProtocol();
    std::string expected_out_string;
    auto out = _impl::ClientProtocol::OutputBuffer();

    expected_out_string = "bind 888234 0 5 1 0\ntoken";
    protocol.make_pbs_bind_message(7, out, 888234, std::string{}, "token", true, false);
    compare_out_string(expected_out_string, out, test_context);

    out.reset();
    expected_out_string = "bind 999123 11 12 0 1\nserver/pathtoken_string";
    protocol.make_pbs_bind_message(8, out, 999123, "server/path", "token_string", false, true);
    compare_out_string(expected_out_string, out, test_context);
}

TEST(Protocol_Codec_Bind_FLX)
{
    auto protocol = _impl::ClientProtocol();
    std::string expected_out_string;
    auto out = _impl::ClientProtocol::OutputBuffer();

    auto json_data = nlohmann::json();
    json_data["valA"] = 123;
    json_data["valB"] = "something";

    out.reset();
    expected_out_string = "bind 345888 0 6 1 0\ntoken2";
    protocol.make_flx_bind_message(8, out, 345888, {}, "token2", true, false);
    compare_out_string(expected_out_string, out, test_context);

    out.reset();
    expected_out_string = "bind 456888 31 7 0 1\n{\"valA\":123,\"valB\":\"something\"}token21";
    protocol.make_flx_bind_message(8, out, 456888, json_data, "token21", false, true);
    compare_out_string(expected_out_string, out, test_context);
}

TEST(Protocol_Codec_Ident_PBS)
{
    auto protocol = _impl::ClientProtocol();
    auto out = _impl::ClientProtocol::OutputBuffer();

    auto file_ident = _impl::ClientProtocol::SaltedFileIdent{999123, 123999};
    auto progress = _impl::ClientProtocol::SyncProgress{{1, 2}, {3, 4}, {5, 6}};

    std::string expected_out_string = "ident 234888 999123 123999 3 4 1 2\n";
    protocol.make_pbs_ident_message(out, 234888, file_ident, progress);
    compare_out_string(expected_out_string, out, test_context);
}

TEST(Protocol_Codec_Ident_FLX)
{
    auto protocol = _impl::ClientProtocol();
    auto out = _impl::ClientProtocol::OutputBuffer();

    auto file_ident = _impl::ClientProtocol::SaltedFileIdent{999234, 234999};
    auto progress = _impl::ClientProtocol::SyncProgress{{3, 4}, {5, 6}, {7, 8}};
    std::string query_string = "{\"table\": \"(key == \"value\")\"}";

    std::string expected_out_string = "ident 888234 999234 234999 5 6 3 4 3 29\n{\"table\": \"(key == \"value\")\"}";
    protocol.make_flx_ident_message(out, 888234, file_ident, progress, 3, query_string);
    compare_out_string(expected_out_string, out, test_context);
}

TEST(Protocol_Codec_Query_Change)
{
    auto protocol = _impl::ClientProtocol();
    auto out = _impl::ClientProtocol::OutputBuffer();

    std::string expected_out_string = "query 238881 5 26\n{\"table\": \"(key < value)\"}";
    protocol.make_query_change_message(out, 238881, 5, "{\"table\": \"(key < value)\"}");
    compare_out_string(expected_out_string, out, test_context);
}

TEST(Protocol_Codec_Server_Parse_FLX_BindIdentQuery)
{
    auto client_protocol = _impl::ClientProtocol();
    auto server_protocol = _impl::ServerProtocol();
    auto out = _impl::ClientProtocol::OutputBuffer();
    ServerProtocolTestConnection connection{*test_context.logger};

    auto bind_json = nlohmann::json();
    bind_json["app_id"] = "barq-test";
    bind_json["platform"] = "unit";
    client_protocol.make_flx_bind_message(8, out, 456888, bind_json, "token21", false, true);
    server_protocol.parse_message_received(connection, std::string_view(out.data(), out.size()));

    CHECK(connection.protocol_error.is_ok());
    CHECK(connection.got_bind);
    CHECK_EQUAL(connection.bind_session_ident, 456888);
    CHECK_EQUAL(connection.bind_path, bind_json.dump());
    CHECK_EQUAL(connection.bind_signed_user_token, "token21");
    CHECK_NOT(connection.bind_need_client_file_ident);
    CHECK(connection.bind_is_subserver);

    out.reset();
    auto file_ident = _impl::ClientProtocol::SaltedFileIdent{999234, 234999};
    auto progress = _impl::ClientProtocol::SyncProgress{{3, 4}, {5, 6}, {7, 8}};
    std::string ident_query = "{\"Order\":\"TRUEPREDICATE\"}";
    client_protocol.make_flx_ident_message(out, 888234, file_ident, progress, 3, ident_query);
    server_protocol.parse_message_received(connection, std::string_view(out.data(), out.size()));

    CHECK(connection.protocol_error.is_ok());
    CHECK(connection.got_flx_ident);
    CHECK_EQUAL(connection.ident_session_ident, 888234);
    CHECK_EQUAL(connection.ident_client_file_ident, 999234);
    CHECK_EQUAL(connection.ident_client_file_ident_salt, 234999);
    CHECK_EQUAL(connection.ident_scan_server_version, 5);
    CHECK_EQUAL(connection.ident_scan_client_version, 6);
    CHECK_EQUAL(connection.ident_latest_server_version, 3);
    CHECK_EQUAL(connection.ident_latest_server_version_salt, 4);
    CHECK_EQUAL(connection.ident_query_version, 3);
    CHECK_EQUAL(connection.ident_query_body, ident_query);

    out.reset();
    std::string query = "{\"Order\":\"owner_id == 'user_0'\"}";
    client_protocol.make_query_change_message(out, 888234, 4, query);
    server_protocol.parse_message_received(connection, std::string_view(out.data(), out.size()));

    CHECK(connection.protocol_error.is_ok());
    CHECK(connection.got_query);
    CHECK_EQUAL(connection.query_session_ident, 888234);
    CHECK_EQUAL(connection.query_version, 4);
    CHECK_EQUAL(connection.query_body, query);
}

TEST(Protocol_Codec_JSON_Error)
{
    auto protocol = _impl::ClientProtocol();
    auto out = _impl::ClientProtocol::OutputBuffer();

    auto json_data = nlohmann::json();
    json_data["valA"] = 123;
    json_data["valB"] = "something";
    std::string json_string = json_data.dump();

    std::string expected_out_string = "json_error 9099 31 234888\n{\"valA\":123,\"valB\":\"something\"}";
    protocol.make_json_error_message(out, 234888, 9099, json_string);
    compare_out_string(expected_out_string, out, test_context);
}

TEST(Protocol_Codec_Server_JSON_Error)
{
    auto protocol = _impl::ServerProtocol();
    auto out = _impl::ServerProtocol::OutputBuffer();

    std::string expected_out_string = "json_error 231 20 234888\n{\"message\":\"undone\"}";
    protocol.make_json_error_message(out, 234888, 231, "{\"message\":\"undone\"}");
    compare_out_string(expected_out_string, out, test_context);
}

TEST(Protocol_Codec_Server_Download_FLX)
{
    auto protocol = _impl::ServerProtocol();
    auto out = _impl::ServerProtocol::OutputBuffer();
    std::string body;

    std::string expected_out_string = "download 234888 9 8 10 11 7 6 5 1 0.42 0 0 0\n";
    protocol.make_download_message(14, out, 234888, 9, 8, 10, 11, 7, 6, 999, 0, body.data(), body.size(), 0, false,
                                   *test_context.logger, true, 5, sync::DownloadBatchState::LastInBatch, 0.42);
    compare_out_string(expected_out_string, out, test_context);

    out.reset();
    expected_out_string = "download 234888 9 8 10 11 7 6 5 0 0.25 0 0 0\n";
    protocol.make_download_message(14, out, 234888, 9, 8, 10, 11, 7, 6, 999, 0, body.data(), body.size(), 0, false,
                                   *test_context.logger, true, 5, sync::DownloadBatchState::MoreToCome, 0.25);
    compare_out_string(expected_out_string, out, test_context);
}

TEST(Protocol_Codec_Client_Parse_Download_FLX)
{
    auto protocol = _impl::ClientProtocol();
    ClientProtocolTestConnection connection{*test_context.logger};

    protocol.parse_message_received(connection, "download 234888 9 8 10 11 7 6 5 1 0.42 0 0 0\n");

    CHECK(connection.protocol_error.is_ok());
    CHECK(connection.got_download);
    CHECK_EQUAL(connection.download_session_ident, 234888);
    CHECK_EQUAL(connection.download_message.progress.download.server_version, 9);
    CHECK_EQUAL(connection.download_message.progress.download.last_integrated_client_version, 8);
    CHECK_EQUAL(connection.download_message.progress.latest_server_version.version, 10);
    CHECK_EQUAL(connection.download_message.progress.latest_server_version.salt, 11);
    CHECK_EQUAL(connection.download_message.progress.upload.client_version, 7);
    CHECK_EQUAL(connection.download_message.progress.upload.last_integrated_server_version, 6);
    CHECK(connection.download_message.query_version);
    CHECK_EQUAL(*connection.download_message.query_version, 5);
    CHECK_EQUAL(connection.download_message.batch_state, sync::DownloadBatchState::LastInBatch);
    CHECK_APPROXIMATELY_EQUAL(connection.download_message.downloadable.as_estimate(), 0.42, 0.0001);
    CHECK(connection.download_message.changesets.empty());
}

TEST(Protocol_Codec_Server_Query_Error)
{
    auto protocol = _impl::ServerProtocol();
    auto out = _impl::ServerProtocol::OutputBuffer();

    std::string expected_out_string = "query_error 225 9 234888 5\nbad query";
    protocol.make_query_error_message(out, 225, "bad query", 234888, 5);
    compare_out_string(expected_out_string, out, test_context);
}

TEST(Protocol_Codec_Test_Command)
{
    auto protocol = _impl::ClientProtocol();
    auto out = _impl::ClientProtocol::OutputBuffer();

    std::string expected_out_string = "test_command 234888 1000 17\nsome test command";
    protocol.make_test_command_message(out, 234888, 1000, "some test command");
    compare_out_string(expected_out_string, out, test_context);
}

TEST(Protocol_Codec_Upload)
{
    auto protocol = _impl::ClientProtocol();
    auto out = _impl::ClientProtocol::OutputBuffer();
    {
        auto upload_message_builder = protocol.make_upload_message_builder(); // Throws
        std::string data1 = "AABBCCDDEEFFGGHHIIJJKKLLMMNNOOPP";
        std::string data2 = "EEFFGGHHIIJJKKLLMMNNOOPPQQRRSSTT";

        std::string expected_out_string =
            "upload 999123 0 122 0 30 17 10\n29 18 259604001718 888123 32 AABBCCDDEEFFGGHHIIJJKKLLMMNNOOPP30 19 "
            "259604001850 888234 32 EEFFGGHHIIJJKKLLMMNNOOPPQQRRSSTT";
        upload_message_builder.add_changeset(29, 18, 259604001718, 888123, BinaryData(data1.c_str(), data1.length()));
        upload_message_builder.add_changeset(30, 19, 259604001850, 888234, BinaryData(data2.c_str(), data2.length()));
        upload_message_builder.make_upload_message(7, out, 999123, 30, 17, 10);
        compare_out_string(expected_out_string, out, test_context);
    }

    {
        out.reset();
        auto upload_message_builder = protocol.make_upload_message_builder(); // Throws
        // Create a changeset that exceeds the compression threshold (1024 bytes)
        std::string data1 = std::string(512, 'A') + std::string(512, 'B') + std::string(512, 'C');
        std::string data2 = std::string(util::format("4 2 259609999999 123999 %1 ", data1.length())) + data1;

        std::vector<char> expected_data;
        std::vector<char> compressed;
        util::compression::CompressMemoryArena cmp_memory_arena;
        CHECK_NOT(
            util::compression::allocate_and_compress(cmp_memory_arena, {data2.c_str(), data2.length()}, compressed));
        std::string expected_out_string =
            util::format("upload 888123 1 %1 %2 4 2 0\n", data2.length(), compressed.size());
        expected_data.insert(expected_data.begin(), expected_out_string.begin(), expected_out_string.end());
        expected_data.insert(expected_data.end(), compressed.begin(), compressed.end());

        upload_message_builder.add_changeset(4, 2, 259609999999, 123999, BinaryData(data1.c_str(), data1.size()));
        upload_message_builder.make_upload_message(7, out, 888123, 4, 2, 0);
        compare_out_string(expected_data, out, test_context);

        // Find the compressed changeset and uncompress - first look for the newline
        auto out_span = out.as_span();
        int nl_break = [&out_span] {
            int i = 0;
            for (auto pos = out_span.begin(); pos != out_span.end(); ++pos, i++) {
                if (*pos == '\n')
                    return i;
            }
            return -1;
        }();
        CHECK_GREATER_EQUAL(nl_break, 0);

        // create a new span with the changeset contents
        auto changeset = out_span.last(out_span.size() - (nl_break + 1));
        CHECK_EQUAL(changeset.size(), compressed.size());
        Buffer<char> decompressed_buf(data2.length());
        CHECK_NOT(util::compression::decompress(changeset, decompressed_buf));
        compare_out_string(data2, decompressed_buf, test_context);
    }
}

TEST(Protocol_Codec_Unbind)
{
    auto protocol = _impl::ClientProtocol();
    auto out = _impl::ClientProtocol::OutputBuffer();

    std::string expected_out_string = "unbind 234888\n";
    protocol.make_unbind_message(out, 234888);
    compare_out_string(expected_out_string, out, test_context);
}

TEST(Protocol_Codec_Mark)
{
    auto protocol = _impl::ClientProtocol();
    auto out = _impl::ClientProtocol::OutputBuffer();

    std::string expected_out_string = "mark 234888 888234\n";
    protocol.make_mark_message(out, 234888, 888234);
    compare_out_string(expected_out_string, out, test_context);
}

TEST(Protocol_Codec_Ping)
{
    auto protocol = _impl::ClientProtocol();
    auto out = _impl::ClientProtocol::OutputBuffer();

    std::string expected_out_string = "ping 1234567890 23\n";
    protocol.make_ping(out, 1234567890, 23);
    compare_out_string(expected_out_string, out, test_context);
}
