#include <barq/impl/transact_log.hpp>
#include <barq/sync/instructions.hpp>

using namespace barq;
using namespace barq::_impl;

namespace barq {
namespace sync {

const InternString InternString::npos = InternString{uint32_t(-1)};


} // namespace sync
} // namespace barq
