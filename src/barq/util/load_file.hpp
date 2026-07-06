#ifndef BARQ_UTIL_LOAD_FILE_HPP
#define BARQ_UTIL_LOAD_FILE_HPP

#include <string>

namespace barq {
namespace util {

// FIXME: These functions ought to be moved to <barq/util/file.hpp> in the
// barq-core repository.
std::string load_file(const std::string& path);

} // namespace util
} // namespace barq

#endif // BARQ_UTIL_LOAD_FILE_HPP
