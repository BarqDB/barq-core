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

#include "compatibility.hpp"

using barq::DB;

namespace compatibility {

DB::DurabilityLevel durability(BarqDurability level)
{
    switch (level) {
        case BarqDurability::Full:
            return DB::durability_Full;
        case BarqDurability::MemOnly:
            return DB::durability_MemOnly;
        case BarqDurability::Async:
            return DB::durability_Async;
    }
    BARQ_ASSERT(false); // unhandled case
    return DB::durability_Full;
}

DB* create_new_shared_group(std::string path, BarqDurability level, const char* key)
{
    return new DB(path, false, durability(level), key);
}

} // namespace compatibility
