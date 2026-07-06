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

#ifndef BARQ_VERSION_HPP
#define BARQ_VERSION_HPP

#include <string>

#ifndef BARQ_VERSION_MAJOR
#include <barq/version_numbers.hpp>
#endif

#define BARQ_PRODUCT_NAME "barq-core"
#define BARQ_VER_CHUNK "[" BARQ_PRODUCT_NAME "-" BARQ_VERSION_STRING "]"

namespace barq {

enum Feature {
    feature_Debug,
    feature_Replication,
};

class StringData;

class Version {
public:
    static int get_major()
    {
        return BARQ_VERSION_MAJOR;
    }
    static int get_minor()
    {
        return BARQ_VERSION_MINOR;
    }
    static int get_patch()
    {
        return BARQ_VERSION_PATCH;
    }
    static StringData get_extra();
    static std::string get_version();
    static bool is_at_least(int major, int minor, int patch, StringData extra);
    static bool is_at_least(int major, int minor, int patch);
    static bool has_feature(Feature feature);
};


} // namespace barq

#endif // BARQ_VERSION_HPP
