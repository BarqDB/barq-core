#include <thread>
#include <mutex>
#include <condition_variable>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <barq/binary_data.hpp>
#include <barq/sync/noinst/server/crypto_server.hpp>
#include <barq/sync/client.hpp>
#include <barq/sync/noinst/server/access_token.hpp>
#include <barq/sync/noinst/server/access_control.hpp>
#include <barq/sync/noinst/server/server_file_access_cache.hpp>
#include <barq/sync/noinst/server/server.hpp>

#include "test.hpp"

using namespace barq;
using namespace barq::util;
using namespace barq::sync;

namespace {

#if !BARQ_MOBILE

const char* example_jwt()
{
    return "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9."
           "eyJhcHBJZCI6ImlvLnJlYWxtLkF1dGgiLCJhY2Nlc3MiOlsiZG93bmxvYWQiLCJ1cGxvYWQiXSwic3ViIjoiZGYyZjE4NjBjMTk1MjFiYjk0"
           "NjM0OTRjOTI1MTYyZjciLCJwYXRoIjoiL2RlZmF1bHQvX19wYXJ0aWFsL2RmMmYxODYwYzE5NTIxYmI5NDYzNDk0YzkyNTE2MmY3LzBlYzNj"
           "NjdlMTFjNzFkYmU1ZTgzYmZiNDE3MTViZmJlMGQ5ODNmODYiLCJzeW5jX2xhYmVsIjoiZGVmYXVsdCIsInNhbHQiOiIyY2FmZjhlMCIsImlh"
           "dCI6MTU2NDczNzY1NiwiZXhwIjo0NzIwNDExNjE1LCJhdWQiOiJyZWFsbSIsImlzcyI6InJlYWxtIiwianRpIjoiYmM3MTlkY2ItOTA2Ny00"
           "ZTQ4LWI1NmItYTQ3MzMxZDNmZDgxIn0.SGFUR8A-"
           "XXn2i7LFGcWuUlrfcPgUYRj58ZClZrjsW7NSiE1tI5zZSbrEL7vyTPtwbMbMe1qMgdoB1ZdSzt-HAB9RCIrRk40XlHw7flb8jk_"
           "q0hdqPnKbxEMz9wWzzUGOshXj2Yso1NVEX0q04k-ndpAODtuMDiU5T_3vF1czUFA-WXOMDr9dpX_Wn8KeEO0uOvb4_1AvDM_"
           "wK3RF5D9IsJGuvE2Sqbq5j2DPGCgTkBsTcKJPQPcgEDC270nSb9SfitzLEzxoQbhF9M82MQJqhfj4ZThImG6ed7hjUIqdgBFuyBQ4WaMQgPD"
           "vA5KRPYymC5owAHBmGht9wpUFzAbnBg";
}

class TestServerHistoryContext : public _impl::ServerHistory::Context {
public:
    std::mt19937_64& server_history_get_random() noexcept override final
    {
        return m_random;
    }

private:
    std::mt19937_64 m_random;
};

TEST(Sync_Auth_JWTAccessToken)
{
    AccessToken tok;
    AccessToken::ParseError error = AccessToken::ParseError::none;

    PKey pk1 = PKey::load_public(test_util::get_test_resource_path() + "test_pubkey2.pem");
    AccessControl ctrl(std::move(pk1));

    AccessToken::Verifier& verifier = ctrl.verifier();
    auto result = AccessToken::parseJWT(StringData(example_jwt()), tok, error, &verifier);

    CHECK(result);
    CHECK(error == AccessToken::ParseError::none);
    CHECK_EQUAL(tok.expires, 4720411615);
    CHECK_EQUAL(tok.identity, "df2f1860c19521bb9463494c925162f7");
    CHECK_EQUAL(tok.sync_label, "default");
}

TEST(Sync_Auth_TenantKeyStoreAcceptsClaimedTenant)
{
    auto keys = std::make_shared<AccessControl::PublicKeyStore>();
    keys->keys["io.realm.Auth"].push_back(PKey::load_public(test_util::get_test_resource_path() + "test_pubkey2.pem"));
    AccessControl ctrl(util::none, keys);

    AccessToken::ParseError error = AccessToken::ParseError::none;
    auto token = ctrl.verify_access_token(StringData(example_jwt()), &error);

    CHECK(token);
    CHECK(error == AccessToken::ParseError::none);
    CHECK_EQUAL(token->app_id, "io.realm.Auth");
}

TEST(Sync_Auth_TenantKeyStoreAcceptsRotatedTenantKey)
{
    auto keys = std::make_shared<AccessControl::PublicKeyStore>();
    keys->keys["io.realm.Auth"].push_back(PKey::load_public(test_util::get_test_resource_path() + "test_pubkey.pem"));
    keys->keys["io.realm.Auth"].push_back(PKey::load_public(test_util::get_test_resource_path() + "test_pubkey2.pem"));
    AccessControl ctrl(util::none, keys);

    AccessToken::ParseError error = AccessToken::ParseError::none;
    auto token = ctrl.verify_access_token(StringData(example_jwt()), &error);

    CHECK(token);
    CHECK(error == AccessToken::ParseError::none);
    CHECK_EQUAL(token->app_id, "io.realm.Auth");
}

TEST(Sync_Auth_TenantKeyStoreRejectsUnknownTenant)
{
    auto keys = std::make_shared<AccessControl::PublicKeyStore>();
    keys->keys["other"].push_back(PKey::load_public(test_util::get_test_resource_path() + "test_pubkey2.pem"));
    AccessControl ctrl(util::none, keys);

    AccessToken::ParseError error = AccessToken::ParseError::none;
    auto token = ctrl.verify_access_token(StringData(example_jwt()), &error);

    CHECK(!token);
    CHECK(error == AccessToken::ParseError::unknown_app_id);
}

TEST(Sync_Auth_TenantKeyStoreRejectsWrongTenantKey)
{
    auto keys = std::make_shared<AccessControl::PublicKeyStore>();
    keys->keys["io.realm.Auth"].push_back(PKey::load_public(test_util::get_test_resource_path() + "test_pubkey.pem"));
    AccessControl ctrl(util::none, keys);

    AccessToken::ParseError error = AccessToken::ParseError::none;
    auto token = ctrl.verify_access_token(StringData(example_jwt()), &error);

    CHECK(!token);
    CHECK(error == AccessToken::ParseError::invalid_signature);
}

TEST(Sync_Auth_ServerConfigRejectsTenantSecretWithoutTenantKeys)
{
    Server::Config config;
    config.tenant_encryption_master_secret = std::vector<char>(32, 'x');

    CHECK_THROW(Server("unused", util::none, std::move(config)), std::invalid_argument);
}

TEST(Sync_Auth_ServerConfigRejectsTenantLimitsWithoutTenantKeys)
{
    Server::Config config;
    config.tenant_limits.max_connections = 1;

    CHECK_THROW(Server("unused", util::none, std::move(config)), std::invalid_argument);
}

TEST(Sync_Auth_TenantOpenFileQuotaEvictsOnlyTenant)
{
    SHARED_GROUP_TEST_PATH(path_a_1);
    SHARED_GROUP_TEST_PATH(path_a_2);
    SHARED_GROUP_TEST_PATH(path_b_1);
    TestServerHistoryContext history_context;
    _impl::ServerFileAccessCache cache(4, *test_context.logger, history_context, util::none, util::none, 1);

    _impl::ServerFileAccessCache::Slot slot_a_1(cache, path_a_1, "/tenant-a/one", false, false);
    _impl::ServerFileAccessCache::Slot slot_a_2(cache, path_a_2, "/tenant-a/two", false, false);
    _impl::ServerFileAccessCache::Slot slot_b_1(cache, path_b_1, "/tenant-b/one", false, false);

    slot_a_1.access();
    slot_b_1.access();
    CHECK(slot_a_1.is_open());
    CHECK(slot_b_1.is_open());

    slot_a_2.access();
    CHECK(!slot_a_1.is_open());
    CHECK(slot_a_2.is_open());
    CHECK(slot_b_1.is_open());

    cache.proper_close_all();
}

TEST(Sync_Auth_TokenPathIsRelativeInsideTenant)
{
    AccessToken token;
    token.access = Privilege::Download | Privilege::None;
    token.path = "/shared";

    AccessControl ctrl(util::none);
    CHECK(ctrl.can(token, Privilege::Read, "shared"));
    CHECK(!ctrl.can(token, Privilege::Read, "other"));
}

TEST(Sync_Auth_TenantEncryptionKeysDiffer)
{
    std::vector<char> master_secret(32, 'x');

    auto key_a_1 = _impl::derive_tenant_encryption_key("tenant-a", master_secret);
    auto key_a_2 = _impl::derive_tenant_encryption_key("tenant-a", master_secret);
    auto key_b = _impl::derive_tenant_encryption_key("tenant-b", master_secret);

    CHECK(key_a_1 == key_a_2);
    CHECK(key_a_1 != key_b);

    std::array<unsigned char, 64> expected = {
        0x3f, 0xe5, 0x05, 0x1b, 0x95, 0xd9, 0x79, 0x2f, 0x1c, 0x65, 0x95, 0x66, 0x00,
        0xbb, 0xac, 0xac, 0xc9, 0x98, 0x2d, 0x91, 0x2d, 0x5a, 0xae, 0xb4, 0xda, 0xe9,
        0xb0, 0xcc, 0x98, 0x11, 0xe5, 0x76, 0xd0, 0xe5, 0xc5, 0x2c, 0xa1, 0x9b, 0xc5,
        0xb3, 0x86, 0xd0, 0x0d, 0x3c, 0x7f, 0xb7, 0x05, 0x5a, 0x4f, 0xfc, 0x7a, 0xb0,
        0x8c, 0x19, 0x60, 0xff, 0xf7, 0xcc, 0x2b, 0x72, 0xc8, 0xa1, 0x13, 0x92,
    };
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK_EQUAL(static_cast<unsigned char>(key_a_1[i]), expected[i]);
    }
}

#endif // !BARQ_MOBILE

} // unnamed namespace
