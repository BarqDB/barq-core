/*************************************************************************
 *
 * Copyright 2016 Realm Inc.
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

#ifndef BARQ_UTIL_TERMINATE_HPP
#define BARQ_UTIL_TERMINATE_HPP

#include <cstdlib>

#include <barq/util/features.h>
#include <barq/util/to_string.hpp>
#include <barq/version.hpp>

#define BARQ_TERMINATE(msg) barq::util::terminate((msg), __FILE__, __LINE__)

namespace barq {
namespace util {

BARQ_NORETURN void terminate(const char* message, const char* file, long line,
                              std::initializer_list<Printable>&& = {}) noexcept;
BARQ_NORETURN void terminate_with_info(const char* message, const char* file, long line,
                                        const char* interesting_names,
                                        std::initializer_list<Printable>&& = {}) noexcept;

// LCOV_EXCL_START
template <class... Ts>
BARQ_NORETURN void terminate(const char* message, const char* file, long line, Ts... infos) noexcept
{
    static_assert(sizeof...(infos) == 2 || sizeof...(infos) == 4 || sizeof...(infos) == 6,
                  "Called barq::util::terminate() with wrong number of arguments");
    terminate(message, file, line, {Printable(infos)...});
}

template <class... Args>
BARQ_NORETURN void terminate_with_info(const char* assert_message, int line, const char* file,
                                        const char* interesting_names, Args&&... interesting_values) noexcept
{
    terminate_with_info(assert_message, file, line, interesting_names, {Printable(interesting_values)...});
}
// LCOV_EXCL_STOP

} // namespace util
} // namespace barq

#endif // BARQ_UTIL_TERMINATE_HPP
