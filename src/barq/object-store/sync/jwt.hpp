////////////////////////////////////////////////////////////////////////////
//
// Copyright 2024 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#ifndef BARQ_OS_JWT_HPP
#define BARQ_OS_JWT_HPP

#include <external/json/json.hpp>

#include <optional>
#include <string>

namespace barq {
// A struct that decodes a given JWT.
struct BarqJWT {
    // The raw encoded token
    std::string token;

    // When the token expires.
    int64_t expires_at = 0;
    // When the token was issued.
    int64_t issued_at = 0;
    // Custom user data embedded in the encoded token.
    std::optional<nlohmann::json> user_data;

    BarqJWT() = default;
    explicit BarqJWT(std::string_view token);
    explicit BarqJWT(const std::string& token)
        : BarqJWT(std::string_view(token))
    {
    }

    static bool validate(std::string_view token);

    bool operator==(const BarqJWT& other) const noexcept
    {
        return token == other.token;
    }
    bool operator!=(const BarqJWT& other) const noexcept
    {
        return token != other.token;
    }

    explicit operator bool() const noexcept
    {
        return !token.empty();
    }
};

} // namespace barq

#endif // BARQ_OS_JWT_HPP
