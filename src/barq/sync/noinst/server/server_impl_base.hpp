
#ifndef BARQ_NOINST_SERVER_IMPL_BASE_HPP
#define BARQ_NOINST_SERVER_IMPL_BASE_HPP

#include <barq/sync/protocol.hpp>

namespace barq {
namespace _impl {

class ServerImplBase {
public:
    static constexpr int get_oldest_supported_protocol_version() noexcept;
};

constexpr int ServerImplBase::get_oldest_supported_protocol_version() noexcept
{
    // See sync::get_current_protocol_version() for information about the
    // individual protocol versions.
    return 2;
}

static_assert(ServerImplBase::get_oldest_supported_protocol_version() >= 1, "");
static_assert(ServerImplBase::get_oldest_supported_protocol_version() <= sync::get_current_protocol_version(), "");

} // namespace _impl
} // namespace barq

#endif // BARQ_NOINST_SERVER_IMPL_BASE_HPP
