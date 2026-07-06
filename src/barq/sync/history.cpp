#include <barq/history.hpp>
#include <barq/sync/noinst/client_history_impl.hpp>

namespace barq::sync {

std::unique_ptr<ClientReplication> make_client_replication()
{
    bool apply_server_changes = true;
    return std::make_unique<ClientReplication>(apply_server_changes); // Throws
}

} // namespace barq::sync
