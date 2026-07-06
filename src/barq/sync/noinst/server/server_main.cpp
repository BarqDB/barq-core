#include <barq/sync/noinst/server/crypto_server.hpp>
#include <barq/sync/noinst/server/server.hpp>
#include <barq/sync/network/network.hpp>
#include <barq/util/file.hpp>
#include <barq/util/logger.hpp>
#include <barq/util/optional.hpp>
#include <barq/version.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

using namespace barq;

namespace {

std::atomic<bool> g_stop_requested{false};

void request_stop(int) noexcept
{
    g_stop_requested.store(true);
}

struct Options {
    std::string host = "127.0.0.1";
    std::string port = "9090";
    std::string root_dir;
    std::string jwt_public_key;
    std::string tls_cert;
    std::string tls_key;
    std::string server_id = "barq-server";
    util::Logger::Level log_level = util::Logger::Level::info;
    bool allow_unsigned_tokens = false;
    bool disable_sync_to_disk = false;
};

void print_usage(std::ostream& out)
{
    out << "Usage:\n"
        << "  barq-server --root-dir DIR --jwt-public-key PEM [options]\n\n"
        << "Options:\n"
        << "  --host HOST                 Listen host. Default: 127.0.0.1\n"
        << "  --port PORT                 Listen port. Default: 9090. Use 0 for a free port.\n"
        << "  --root-dir DIR              Directory for server Barq files.\n"
        << "  --jwt-public-key PEM        RSA public key for access token checks.\n"
        << "  --allow-unsigned-tokens     Dev/test only. Do not verify token signatures.\n"
        << "  --tls-cert PEM              TLS certificate chain file.\n"
        << "  --tls-key PEM               TLS private key file.\n"
        << "  --server-id ID              Server id. Default: barq-server\n"
        << "  --log-level LEVEL           all, trace, debug, detail, info, warn, error, fatal, off.\n"
        << "  --disable-sync-to-disk      Dev/test only. Keep synced data off disk.\n"
        << "  --help                      Show this help.\n";
}

bool read_value(int& i, int argc, char* argv[], std::string& out, const char* name)
{
    if (i + 1 >= argc) {
        std::cerr << name << " needs a value\n";
        return false;
    }
    out = argv[++i];
    return true;
}

bool parse_log_level(const std::string& value, util::Logger::Level& level)
{
    std::istringstream in(value);
    in >> level;
    return !in.fail() && in.eof();
}

bool parse_args(int argc, char* argv[], Options& options)
{
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            std::exit(0);
        }
        if (arg == "--host") {
            if (!read_value(i, argc, argv, options.host, "--host"))
                return false;
        }
        else if (arg == "--port") {
            if (!read_value(i, argc, argv, options.port, "--port"))
                return false;
        }
        else if (arg == "--root-dir") {
            if (!read_value(i, argc, argv, options.root_dir, "--root-dir"))
                return false;
        }
        else if (arg == "--jwt-public-key") {
            if (!read_value(i, argc, argv, options.jwt_public_key, "--jwt-public-key"))
                return false;
        }
        else if (arg == "--tls-cert") {
            if (!read_value(i, argc, argv, options.tls_cert, "--tls-cert"))
                return false;
        }
        else if (arg == "--tls-key") {
            if (!read_value(i, argc, argv, options.tls_key, "--tls-key"))
                return false;
        }
        else if (arg == "--server-id") {
            if (!read_value(i, argc, argv, options.server_id, "--server-id"))
                return false;
        }
        else if (arg == "--log-level") {
            std::string value;
            if (!read_value(i, argc, argv, value, "--log-level"))
                return false;
            if (!parse_log_level(value, options.log_level)) {
                std::cerr << "Unknown log level: " << value << "\n";
                return false;
            }
        }
        else if (arg == "--allow-unsigned-tokens") {
            options.allow_unsigned_tokens = true;
        }
        else if (arg == "--disable-sync-to-disk") {
            options.disable_sync_to_disk = true;
        }
        else {
            std::cerr << "Unknown option: " << arg << "\n";
            return false;
        }
    }

    if (options.root_dir.empty()) {
        std::cerr << "--root-dir is required\n";
        return false;
    }
    if (options.jwt_public_key.empty() && !options.allow_unsigned_tokens) {
        std::cerr << "--jwt-public-key is required unless --allow-unsigned-tokens is set\n";
        return false;
    }
    if (!options.jwt_public_key.empty() && options.allow_unsigned_tokens) {
        std::cerr << "Use either --jwt-public-key or --allow-unsigned-tokens, not both\n";
        return false;
    }
    if (options.tls_cert.empty() != options.tls_key.empty()) {
        std::cerr << "--tls-cert and --tls-key must be passed together\n";
        return false;
    }

    return true;
}

std::string route_host(const std::string& configured_host, const sync::network::Endpoint& endpoint)
{
    if (configured_host == "0.0.0.0" || configured_host == "::")
        return "localhost";

    auto address = endpoint.address();
    std::ostringstream out;
    out << address;
    auto host = out.str();
    if (address.is_ip_v6())
        return "[" + host + "]";
    return host;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        Options options;
        if (!parse_args(argc, argv, options)) {
            print_usage(std::cerr);
            return EXIT_FAILURE;
        }

        auto root_dir = util::File::resolve(options.root_dir, ".");
        util::make_dir_recursive(root_dir);

        util::Optional<sync::PKey> public_key = util::none;
        if (!options.allow_unsigned_tokens)
            public_key = sync::PKey::load_public(options.jwt_public_key);

        auto logger = std::make_shared<util::StderrLogger>(options.log_level);

        sync::Server::Config config;
        config.id = options.server_id;
        config.listen_address = options.host;
        config.listen_port = options.port;
        config.logger = logger;
        config.ssl = !options.tls_cert.empty();
        config.ssl_certificate_path = options.tls_cert;
        config.ssl_certificate_key_path = options.tls_key;
        config.disable_sync_to_disk = options.disable_sync_to_disk;

        sync::Server server(root_dir, std::move(public_key), std::move(config));
        server.start();

        std::signal(SIGINT, request_stop);
        std::signal(SIGTERM, request_stop);

        std::thread stop_thread([&server] {
            while (!g_stop_requested.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            server.stop();
        });

        const auto endpoint = server.listen_endpoint();
        const char* scheme = options.tls_cert.empty() ? "ws" : "wss";
        std::cout << "Barq sync server " << BARQ_VERSION_STRING << " listening on " << scheme << "://"
                  << route_host(options.host, endpoint) << ":" << endpoint.port() << "/barq-sync" << std::endl;

        std::exception_ptr run_error;
        try {
            server.run();
        }
        catch (...) {
            run_error = std::current_exception();
        }
        g_stop_requested.store(true);
        stop_thread.join();
        if (run_error)
            std::rethrow_exception(run_error);
        return EXIT_SUCCESS;
    }
    catch (const std::exception& e) {
        g_stop_requested.store(true);
        std::cerr << "barq-server: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
