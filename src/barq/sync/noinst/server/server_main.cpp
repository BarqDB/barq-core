#include <barq/sync/noinst/server/crypto_server.hpp>
#include <barq/sync/noinst/server/access_control.hpp>
#include <barq/sync/noinst/server/server_dir.hpp>
#include <barq/sync/noinst/server/server.hpp>
#include <barq/sync/network/network.hpp>
#include <barq/util/file.hpp>
#include <barq/util/load_file.hpp>
#include <barq/util/logger.hpp>
#include <barq/util/optional.hpp>
#include <barq/version.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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
    std::string jwt_public_key_dir;
    std::string tenant_encryption_master_key;
    std::string tls_cert;
    std::string tls_key;
    std::string server_id = "barq-server";
    util::Logger::Level log_level = util::Logger::Level::info;
    std::size_t tenant_max_connections = 0;
    std::size_t tenant_max_files = 0;
    std::size_t tenant_max_open_files = 0;
    std::uintmax_t tenant_max_storage_bytes = 0;
    bool allow_unsigned_tokens = false;
    bool disable_sync_to_disk = false;
    bool enable_flx_sync = false;
    std::vector<sync::Server::Config::FLXRule> flx_rules;
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
        << "  --jwt-public-key-dir DIR    Per-tenant public keys. Use <tenant_id>.pem or <tenant_id>/*.pem.\n"
        << "  --tenant-encryption-master-key FILE\n"
        << "                              Master secret file for per-tenant DB keys.\n"
        << "  --tenant-max-connections N  Max concurrent sync connections per tenant. 0 = unlimited.\n"
        << "  --tenant-max-files N        Max Barq files per tenant. 0 = unlimited.\n"
        << "  --tenant-max-open-files N   Max open Barq files per tenant per server thread. 0 = unlimited.\n"
        << "  --tenant-max-storage-bytes N\n"
        << "                              Max current on-disk bytes per tenant. 0 = unlimited.\n"
        << "  --enable-flx               Enable Flexible Sync websocket negotiation.\n"
        << "  --flx-owner-rule TABLE:FIELD\n"
        << "                              Allow owner rows where FIELD equals token identity.\n"
        << "  --flx-public-readonly-rule TABLE\n"
        << "                              Allow read-only public rows for TABLE.\n"
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

bool parse_flx_owner_rule(const std::string& value, sync::Server::Config::FLXRule& rule)
{
    std::size_t pos = value.find(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 == value.size())
        return false;

    rule.table = value.substr(0, pos);
    rule.mode = sync::Server::Config::FLXRule::Mode::Owner;
    rule.owner_field = value.substr(pos + 1);
    return true;
}

template <class T>
bool parse_unsigned_number(const std::string& value, T& out)
{
    if (value.empty() || value.front() == '-')
        return false;
    std::istringstream in(value);
    T parsed = 0;
    in >> parsed;
    if (in.fail() || !in.eof())
        return false;
    out = parsed;
    return true;
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
        else if (arg == "--jwt-public-key-dir") {
            if (!read_value(i, argc, argv, options.jwt_public_key_dir, "--jwt-public-key-dir"))
                return false;
        }
        else if (arg == "--tenant-encryption-master-key") {
            if (!read_value(i, argc, argv, options.tenant_encryption_master_key, "--tenant-encryption-master-key"))
                return false;
        }
        else if (arg == "--tenant-max-connections") {
            std::string value;
            if (!read_value(i, argc, argv, value, "--tenant-max-connections"))
                return false;
            if (!parse_unsigned_number(value, options.tenant_max_connections)) {
                std::cerr << "Invalid --tenant-max-connections: " << value << "\n";
                return false;
            }
        }
        else if (arg == "--tenant-max-files") {
            std::string value;
            if (!read_value(i, argc, argv, value, "--tenant-max-files"))
                return false;
            if (!parse_unsigned_number(value, options.tenant_max_files)) {
                std::cerr << "Invalid --tenant-max-files: " << value << "\n";
                return false;
            }
        }
        else if (arg == "--tenant-max-open-files") {
            std::string value;
            if (!read_value(i, argc, argv, value, "--tenant-max-open-files"))
                return false;
            if (!parse_unsigned_number(value, options.tenant_max_open_files)) {
                std::cerr << "Invalid --tenant-max-open-files: " << value << "\n";
                return false;
            }
        }
        else if (arg == "--tenant-max-storage-bytes") {
            std::string value;
            if (!read_value(i, argc, argv, value, "--tenant-max-storage-bytes"))
                return false;
            if (!parse_unsigned_number(value, options.tenant_max_storage_bytes)) {
                std::cerr << "Invalid --tenant-max-storage-bytes: " << value << "\n";
                return false;
            }
        }
        else if (arg == "--enable-flx") {
            options.enable_flx_sync = true;
        }
        else if (arg == "--flx-owner-rule") {
            std::string value;
            if (!read_value(i, argc, argv, value, "--flx-owner-rule"))
                return false;
            sync::Server::Config::FLXRule rule;
            if (!parse_flx_owner_rule(value, rule)) {
                std::cerr << "Invalid --flx-owner-rule: " << value << " (expected TABLE:FIELD)\n";
                return false;
            }
            options.flx_rules.push_back(std::move(rule));
        }
        else if (arg == "--flx-public-readonly-rule") {
            sync::Server::Config::FLXRule rule;
            if (!read_value(i, argc, argv, rule.table, "--flx-public-readonly-rule"))
                return false;
            rule.mode = sync::Server::Config::FLXRule::Mode::PublicReadOnly;
            options.flx_rules.push_back(std::move(rule));
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
    int auth_modes = 0;
    if (!options.jwt_public_key.empty())
        ++auth_modes;
    if (!options.jwt_public_key_dir.empty())
        ++auth_modes;
    if (options.allow_unsigned_tokens)
        ++auth_modes;
    if (auth_modes == 0) {
        std::cerr << "--jwt-public-key or --jwt-public-key-dir is required unless --allow-unsigned-tokens is set\n";
        return false;
    }
    if (auth_modes > 1) {
        std::cerr << "Use only one of --jwt-public-key, --jwt-public-key-dir, or --allow-unsigned-tokens\n";
        return false;
    }
    if (!options.tenant_encryption_master_key.empty() && options.jwt_public_key_dir.empty()) {
        std::cerr << "--tenant-encryption-master-key requires --jwt-public-key-dir\n";
        return false;
    }
    bool has_tenant_limits = options.tenant_max_connections != 0 || options.tenant_max_files != 0 ||
                             options.tenant_max_open_files != 0 || options.tenant_max_storage_bytes != 0;
    if (has_tenant_limits && options.jwt_public_key_dir.empty()) {
        std::cerr << "Tenant limits require --jwt-public-key-dir\n";
        return false;
    }
    if (options.tls_cert.empty() != options.tls_key.empty()) {
        std::cerr << "--tls-cert and --tls-key must be passed together\n";
        return false;
    }
    if (!options.flx_rules.empty() && !options.enable_flx_sync) {
        std::cerr << "--flx-owner-rule and --flx-public-readonly-rule require --enable-flx\n";
        return false;
    }

    return true;
}

std::shared_ptr<sync::AccessControl::PublicKeyStore> load_tenant_public_keys(const std::string& dir)
{
    auto store = std::make_shared<sync::AccessControl::PublicKeyStore>();

    auto add_key = [&](const std::string& tenant_id, const std::string& path) {
        if (!_impl::valid_virt_path_segment(tenant_id)) {
            throw std::runtime_error("Invalid tenant public key name: " + tenant_id);
        }
        store->keys[tenant_id].push_back(sync::PKey::load_public(path)); // Throws
    };

    util::DirScanner scanner{dir}; // Throws
    std::string name;
    while (scanner.next(name)) { // Throws
        std::string path = util::File::resolve(name, dir); // Throws
        if (util::File::is_dir(path)) {
            if (!_impl::valid_virt_path_segment(name)) {
                throw std::runtime_error("Invalid tenant public key directory: " + name);
            }
            util::DirScanner tenant_scanner{path}; // Throws
            std::string key_name;
            while (tenant_scanner.next(key_name)) { // Throws
                if (!StringData{key_name}.ends_with(".pem"))
                    continue;
                std::string key_path = util::File::resolve(key_name, path); // Throws
                if (!util::File::is_dir(key_path))
                    add_key(name, key_path); // Throws
            }
            continue;
        }

        if (!StringData{name}.ends_with(".pem"))
            continue;

        std::string tenant_id = name.substr(0, name.size() - 4); // Throws
        add_key(tenant_id, path);                                // Throws
    }

    if (store->keys.empty()) {
        throw std::runtime_error("No tenant public keys found in: " + dir);
    }
    return store;
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
        if (!options.jwt_public_key.empty())
            public_key = sync::PKey::load_public(options.jwt_public_key);

        std::shared_ptr<sync::AccessControl::PublicKeyStore> tenant_public_keys;
        if (!options.jwt_public_key_dir.empty())
            tenant_public_keys = load_tenant_public_keys(options.jwt_public_key_dir); // Throws

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
        config.tenant_public_keys = std::move(tenant_public_keys);
        config.tenant_limits.max_connections = options.tenant_max_connections;
        config.tenant_limits.max_files = options.tenant_max_files;
        config.tenant_limits.max_open_files = options.tenant_max_open_files;
        config.tenant_limits.max_storage_bytes = options.tenant_max_storage_bytes;
        config.enable_flx_sync = options.enable_flx_sync;
        config.flx_rules = std::move(options.flx_rules);
        if (!options.tenant_encryption_master_key.empty()) {
            auto secret = util::load_file(options.tenant_encryption_master_key); // Throws
            config.tenant_encryption_master_secret = std::vector<char>(secret.begin(), secret.end()); // Throws
        }

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
