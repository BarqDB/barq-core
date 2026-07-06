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

#include <algorithm>

#include "verified_string.hpp"
#include "barq/array_key.hpp"

using namespace barq;
using namespace barq::test_util;


VerifiedString::VerifiedString()
    : u(Allocator::get_default())
{
    u.create();
}


VerifiedString::~VerifiedString()
{
    u.destroy();
}

void VerifiedString::verify_neighbours(size_t ndx)
{
    if (v.size() > ndx)
        BARQ_ASSERT(v[ndx] == u.get(ndx));

    if (ndx > 0)
        BARQ_ASSERT(v[ndx - 1] == u.get(ndx - 1));

    if (v.size() > ndx + 1)
        BARQ_ASSERT(v[ndx + 1] == u.get(ndx + 1));
}

void VerifiedString::add(StringData value)
{
    v.push_back(value);
    u.add(value);
    BARQ_ASSERT(v.size() == u.size());
    verify_neighbours(v.size());
    BARQ_ASSERT(conditional_verify());
}


void VerifiedString::insert(size_t ndx, StringData value)
{
    v.insert(v.begin() + ndx, value);
    u.insert(ndx, value);
    BARQ_ASSERT(v.size() == u.size());
    verify_neighbours(ndx);
    BARQ_ASSERT(conditional_verify());
}


StringData VerifiedString::get(size_t ndx)
{
    BARQ_ASSERT(v[ndx] == u.get(ndx));
    return v[ndx];
}

void VerifiedString::set(size_t ndx, StringData value)
{
    v[ndx] = value;
    u.set(ndx, value);
    verify_neighbours(ndx);
    BARQ_ASSERT(conditional_verify());
}

void VerifiedString::erase(size_t ndx)
{
    v.erase(v.begin() + ndx);
    u.erase(ndx);
    BARQ_ASSERT(v.size() == u.size());
    verify_neighbours(ndx);
    BARQ_ASSERT(conditional_verify());
}

void VerifiedString::clear()
{
    v.clear();
    u.clear();
    BARQ_ASSERT(v.size() == u.size());
    BARQ_ASSERT(conditional_verify());
}

size_t VerifiedString::find_first(StringData value)
{
    std::vector<std::string>::iterator it = std::find(v.begin(), v.end(), value);
    size_t ndx = std::distance(v.begin(), it);
    size_t index2 = u.find_first(value);
    static_cast<void>(index2);
    BARQ_ASSERT(ndx == index2 || (it == v.end() && index2 == size_t(-1)));
    return ndx;
}

size_t VerifiedString::size()
{
    BARQ_ASSERT(v.size() == u.size());
    return v.size();
}

bool VerifiedString::verify()
{
    BARQ_ASSERT(u.size() == v.size());
    if (u.size() != v.size())
        return false;

    for (size_t t = 0; t < v.size(); ++t) {
        BARQ_ASSERT(v[t] == u.get(t));
        if (v[t] != u.get(t))
            return false;
    }
    return true;
}

// makes it run amortized the same time complexity as original, even though the row count grows
bool VerifiedString::conditional_verify()
{
    if ((uint64_t(rand()) * uint64_t(rand())) % (v.size() / 10 + 1) == 0) {
        return verify();
    }
    else {
        return true;
    }
}
