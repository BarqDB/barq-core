////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2026 the Barq authors
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

// Standalone proof that Barq DB at-rest encryption works.
//
// 1. Control: write a known marker into an UNencrypted realm and confirm the
//    marker is readable in the raw file bytes (proves the byte scan works).
// 2. Write the same marker into an ENCRYPTED realm and confirm the marker is
//    NOT present in the raw bytes (on-disk data is ciphertext).
// 3. Reopen the encrypted realm with the correct key and read the marker back.
// 4. Confirm opening with a WRONG key fails.
// 5. Confirm opening with NO key fails.
//
// Exit code 0 = PASS. Usage: barq-encryption-test <work-dir>

#include <barq/db.hpp>
#include <barq/db_options.hpp>
#include <barq/transaction.hpp>
#include <barq/table.hpp>
#include <barq/obj.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace barq;

namespace {

const char* MARKER = "SUPER_SECRET_MARKER_9F3A7C1E";

bool file_contains(const std::string& path, const std::string& needle)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end()) != bytes.end();
}

void write_marker(const std::string& path, const char* key)
{
    DBOptions opts(key);
    auto db = DB::create(path, opts);
    auto tr = db->start_write();
    auto table = tr->add_table_with_primary_key("class_Secret", type_Int, "_id");
    auto col = table->add_column(type_String, "data");
    table->create_object_with_primary_key(Mixed(int64_t(1))).set(col, StringData(MARKER));
    tr->commit();
    db->close();
}

} // namespace

int main(int argc, char* argv[])
{
    std::cout << std::unitbuf;
    if (argc < 2) {
        std::cerr << "usage: barq-encryption-test <work-dir>\n";
        return 2;
    }
    const std::string dir = argv[1];
    const std::string plain_path = dir + "/plain.barq";
    const std::string enc_path = dir + "/encrypted.barq";

    std::array<char, 64> key{};
    for (int i = 0; i < 64; ++i)
        key[i] = static_cast<char>(i * 7 + 3);
    std::array<char, 64> wrong_key = key;
    wrong_key[0] = static_cast<char>(wrong_key[0] ^ 0xFF);

    // 1. Control: unencrypted realm -- marker MUST be visible in raw bytes.
    write_marker(plain_path, nullptr);
    bool plain_has_marker = file_contains(plain_path, MARKER);
    std::cout << "[1] control (unencrypted): marker found in raw bytes = "
              << (plain_has_marker ? "YES" : "NO") << "\n";
    if (!plain_has_marker) {
        std::cerr << "FAIL: control broken -- scan cannot even find plaintext\n";
        return 1;
    }

    // 2. Encrypted realm -- marker MUST NOT appear in raw bytes.
    write_marker(enc_path, key.data());
    bool enc_has_marker = file_contains(enc_path, MARKER);
    std::cout << "[2] encrypted: marker found in raw bytes = "
              << (enc_has_marker ? "YES (LEAK!)" : "NO (ciphertext)") << "\n";
    if (enc_has_marker) {
        std::cerr << "FAIL: plaintext leaked to disk in encrypted realm\n";
        return 1;
    }

    // 3. Reopen encrypted with the correct key -> read the marker back.
    {
        DBOptions opts(key.data());
        auto db = DB::create(enc_path, opts);
        auto rt = db->start_read();
        auto table = rt->get_table("class_Secret");
        auto value = table->get_object(0).get<StringData>(table->get_column_key("data"));
        std::cout << "[3] reopened with correct key, decrypted value = '" << value << "'\n";
        if (value != MARKER) {
            std::cerr << "FAIL: decrypted value does not match\n";
            return 1;
        }
        // The read transaction (rt) and db close via RAII at end of scope, in
        // that order -- calling db->close() here would throw (open read txn).
    }

    // 4. Wrong key -> must fail.
    bool wrong_rejected = false;
    try {
        DBOptions opts(wrong_key.data());
        auto db = DB::create(enc_path, opts);
    }
    catch (const std::exception& e) {
        wrong_rejected = true;
        std::cout << "[4] wrong key rejected: " << e.what() << "\n";
    }
    if (!wrong_rejected) {
        std::cerr << "FAIL: a wrong key opened the encrypted realm\n";
        return 1;
    }

    // 5. No key -> must fail.
    bool nokey_rejected = false;
    try {
        auto db = DB::create(enc_path); // default options, no encryption key
    }
    catch (const std::exception& e) {
        nokey_rejected = true;
        std::cout << "[5] no key rejected: " << e.what() << "\n";
    }
    if (!nokey_rejected) {
        std::cerr << "FAIL: encrypted realm opened with no key\n";
        return 1;
    }

    std::cout << "ENCRYPTION: PASS (on-disk ciphertext, correct-key round-trip, wrong/no-key rejected)\n";
    return 0;
}
