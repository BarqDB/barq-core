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

#ifndef BARQ_UTIL_MISC_ERRORS_HPP
#define BARQ_UTIL_MISC_ERRORS_HPP

#include <system_error>


namespace barq {
namespace util {
namespace error {

enum misc_errors {
    unknown = 1,
};

std::error_code make_error_code(misc_errors);
const std::error_category& misc_error_category();

} // namespace error
} // namespace util
} // namespace barq

namespace std {

template <>
class is_error_code_enum<barq::util::error::misc_errors> {
public:
    static const bool value = true;
};

} // namespace std

#endif // BARQ_UTIL_MISC_ERRORS_HPP
