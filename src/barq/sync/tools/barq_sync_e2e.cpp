// Standalone end-to-end sync smoke test.
//
// Connects two independent sync clients (a writer and a reader), each with its
// own local Barq file, to a running `barq-server` over the network and verifies
// that objects written by one client propagate through the server to the other
// in both directions. Exit code 0 = PASS, non-zero = FAIL.
//
// Usage: barq-sync-e2e <server-port> <work-dir>

#include <barq/db.hpp>
#include <barq/transaction.hpp>
#include <barq/table.hpp>
#include <barq/obj.hpp>

#include <barq/sync/client.hpp>
#include <barq/sync/protocol.hpp>
#include <barq/sync/network/default_socket.hpp>
#include <barq/sync/noinst/client_history_impl.hpp>

#include <barq/util/logger.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

using namespace barq;

namespace {

// identity="test", access=["download","upload"], app_id="io.barq.Test".
// Signed with test/test_pubkey.pem's matching private key (same token the sync
// test suite uses).
const char g_signed_test_user_token[] =
    "ewogICAgImlkZW50aXR5IjogInRlc3QiLAogICAgImFjY2VzcyI6IFsiZG93bmxvYWQiLCAidXBsb2FkIl0sCiAgICAidGlt"
    "ZXN0YW1wIjogMTQ1NTUzMDYxNCwKICAgICJleHBpcmVzIjogbnVsbCwKICAgICJhcHBfaWQiOiAiaW8uYmFycS5UZXN0Igp9"
    "Cg==:gf2EBD/k2ZNQYFO07x2dRICigg/sWC//YODgq47tArdEW3wcalUImJQt6r7gnowKfehb63XYn6bM3aNUdQPGy4wqVTa"
    "1Xa1g0Q897+XNK0DIe3Zb4bB/tfcdUbzmaHQDJ6n2Bns19KEdWSuOxz9JjYjTraKMbzXvfDV5xjiufZ57wYJs5Ba743ijnPY"
    "En5RE2vY/B2G8mVMN+6aWUoBetnTOxKNylK5qNKdb1tFEZ7Be5199O9O10FKaFO7p8W0nQtGwkrjbTo8Y4IziGgs+2Lha60f"
    "/n1NSoNa3i8lL5Hi+Z7tHeufFkSpnDDrzcq+TDCGaj3GI5LhK2nbU03SAcQ==";

const char* g_server_path = "/e2e";

sync::Session::Config make_session_config(sync::port_type port, const std::shared_ptr<util::Logger>& logger)
{
    sync::Session::Config config;
    config.server_address = "localhost";
    config.server_port = port;
    config.protocol_envelope = sync::ProtocolEnvelope::barq;
    config.service_identifier = "/barq-sync";
    config.barq_identifier = g_server_path;
    config.signed_user_token = g_signed_test_user_token;
    config.connection_state_change_listener = [logger](sync::ConnectionState state,
                                                       std::optional<sync::SessionErrorInfo> error) {
        if (state == sync::ConnectionState::disconnected && error) {
            logger->error("sync disconnected: %1 (fatal=%2)", error->status, error->is_fatal);
            if (error->is_fatal) {
                // A fatal session error (e.g. the server denied the access token)
                // never completes the sync waits, so report it and exit cleanly.
                std::cerr << "REJECTED by server: " << error->status << "\n";
                std::exit(3);
            }
        }
    };
    return config;
}

struct SyncClient {
    std::shared_ptr<sync::websocket::DefaultSocketProvider> provider;
    std::unique_ptr<sync::Client> client;

    SyncClient(const std::shared_ptr<util::Logger>& logger, const std::string& agent)
    {
        provider = std::make_shared<sync::websocket::DefaultSocketProvider>(logger, agent);
        sync::Client::Config config;
        config.socket_provider = provider;
        config.logger = logger;
        client = std::make_unique<sync::Client>(std::move(config));
    }
};

StringData read_value(const TransactionRef& rt, const char* pk, ColKey& value_col_out)
{
    auto table = rt->get_table("class_Item");
    value_col_out = table->get_column_key("value");
    return table->get_object_with_primary_key(Mixed(StringData(pk))).get<StringData>(value_col_out);
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc < 3) {
        std::cerr << "usage: barq-sync-e2e <server-port> <work-dir>\n";
        return 2;
    }
    const auto port = static_cast<sync::port_type>(std::stoi(argv[1]));
    const std::string work_dir = argv[2];
    std::cout << std::unitbuf; // flush every write so progress survives under redirection

    auto level = std::getenv("BARQ_E2E_TRACE") ? util::Logger::Level::trace : util::Logger::Level::error;
    auto logger = std::make_shared<util::StderrLogger>(level);

    SyncClient writer(logger, "barq-e2e-writer");
    SyncClient reader(logger, "barq-e2e-reader");

    auto writer_db = DB::create(sync::make_client_replication(), work_dir + "/writer.barq");
    auto reader_db = DB::create(sync::make_client_replication(), work_dir + "/reader.barq");

    // ---- Direction 1: writer -> server -> reader ----------------------------
    // Establish the session (a full server round-trip) BEFORE writing, so its
    // upload baseline is the empty file and the new object is tracked as a
    // change to upload -- not adopted as already-synced state.
    sync::Session writer_session(*writer.client, writer_db, nullptr, nullptr, make_session_config(port, logger));
    if (!writer_session.wait_for_download_complete_or_client_stopped()) {
        std::cerr << "FAIL: writer connect interrupted\n";
        return 1;
    }
    {
        auto tr = writer_db->start_write();
        auto table = tr->add_table_with_primary_key("class_Item", type_String, "_id");
        auto value_col = table->add_column(type_String, "value");
        table->create_object_with_primary_key(Mixed(StringData("item1"))).set(value_col, StringData("hello from writer"));
        auto version = tr->commit();
        // Announce the local write to the sync session so it gets uploaded.
        writer_session.nonsync_transact_notify(version);
    }
    if (!writer_session.wait_for_upload_complete_or_client_stopped()) {
        std::cerr << "FAIL: writer upload interrupted\n";
        return 1;
    }
    std::cout << "[1] writer uploaded 'item1' to server path " << g_server_path << "\n";

    sync::Session reader_session(*reader.client, reader_db, nullptr, nullptr, make_session_config(port, logger));
    if (!reader_session.wait_for_download_complete_or_client_stopped()) {
        std::cerr << "FAIL: reader download interrupted\n";
        return 1;
    }
    {
        auto rt = reader_db->start_read();
        auto table = rt->get_table("class_Item");
        if (!table || table->size() != 1) {
            std::cerr << "FAIL: reader expected 1 object, got " << (table ? table->size() : 0) << "\n";
            return 1;
        }
        ColKey value_col;
        auto value = read_value(rt, "item1", value_col);
        std::cout << "[2] reader downloaded 'item1' value = '" << value << "'\n";
        if (value != "hello from writer") {
            std::cerr << "FAIL: reader value mismatch\n";
            return 1;
        }
    }

    // ---- Direction 2: reader -> server -> writer ----------------------------
    // The reader session is now fully established, so announcing the new local
    // change via nonsync_transact_notify() reliably triggers its upload.
    {
        auto tr = reader_db->start_write();
        auto table = tr->get_table("class_Item");
        auto value_col = table->get_column_key("value");
        table->create_object_with_primary_key(Mixed(StringData("item2"))).set(value_col, StringData("hello back from reader"));
        auto version = tr->commit();
        reader_session.nonsync_transact_notify(version);
    }
    if (!reader_session.wait_for_upload_complete_or_client_stopped()) {
        std::cerr << "FAIL: reader upload interrupted\n";
        return 1;
    }
    std::cout << "[3] reader uploaded 'item2' back to server\n";

    if (!writer_session.wait_for_download_complete_or_client_stopped()) {
        std::cerr << "FAIL: writer download interrupted\n";
        return 1;
    }
    {
        auto rt = writer_db->start_read();
        auto table = rt->get_table("class_Item");
        if (!table || table->size() != 2) {
            std::cerr << "FAIL: writer expected 2 objects, got " << (table ? table->size() : 0) << "\n";
            return 1;
        }
        ColKey value_col;
        auto value = read_value(rt, "item2", value_col);
        std::cout << "[4] writer downloaded 'item2' value = '" << value << "'\n";
        if (value != "hello back from reader") {
            std::cerr << "FAIL: writer value mismatch\n";
            return 1;
        }
    }

    std::cout << "E2E SYNC: PASS (bidirectional round-trip through barq-server)\n";
    // Clean teardown via RAII: the sessions destruct first (UNBIND), then the
    // clients and their socket-provider event loops.
    return 0;
}
