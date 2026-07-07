#include <barq/sync/noinst/server/access_control.hpp>

using namespace barq;
using namespace barq::sync;

namespace {

class SingleKeyVerifier final : public AccessToken::Verifier {
public:
    explicit SingleKeyVerifier(const PKey& public_key)
        : m_public_key{public_key}
    {
    }

    bool verify(BinaryData access_token, BinaryData signature) const override final
    {
        return m_public_key.verify(access_token, signature); // Throws
    }

private:
    const PKey& m_public_key;
};

StringData without_leading_slash(const BarqFileIdent& path) noexcept
{
    StringData data{path};
    if (data.size() > 0 && data[0] == '/')
        return data.substr(1);
    return data;
}

bool token_path_matches(const AccessToken& token, const BarqFileIdent& barq_file) noexcept
{
    if (!token.path)
        return true;
    return without_leading_slash(*token.path) == without_leading_slash(barq_file);
}

} // unnamed namespace

struct AccessControl::Impl final : public AccessToken::Verifier {
    util::Optional<PKey> m_public_key;
    std::shared_ptr<const PublicKeyStore> m_tenant_public_keys;

    Impl(util::Optional<PKey> public_key, std::shared_ptr<const PublicKeyStore> tenant_public_keys)
        : m_public_key(std::move(public_key))
        , m_tenant_public_keys(std::move(tenant_public_keys))
    {
    }

    // Overriding members of AccessToken::Verifier
    bool verify(BinaryData access_token, BinaryData signature) const override final
    {
        BARQ_ASSERT(m_public_key);
        return m_public_key->verify(access_token, signature); // Throws
    }
};

AccessControl::AccessControl(util::Optional<PKey> public_key,
                             std::shared_ptr<const PublicKeyStore> tenant_public_keys)
    : m_impl(new Impl(std::move(public_key), std::move(tenant_public_keys)))
{
}

AccessControl::~AccessControl() {}

util::Optional<AccessToken> AccessControl::verify_access_token(StringData signed_token,
                                                               AccessToken::ParseError* out_error) const
{
    AccessToken::ParseError error;
    AccessToken token;
    if (m_impl->m_tenant_public_keys) {
        if (!AccessToken::parse_unverified(signed_token, token, error)) {
            if (out_error)
                *out_error = error;
            return util::none;
        }
        if (token.app_id.empty()) {
            if (out_error)
                *out_error = AccessToken::ParseError::missing_app_id;
            return util::none;
        }
        auto keys = m_impl->m_tenant_public_keys->keys.find(token.app_id);
        if (keys == m_impl->m_tenant_public_keys->keys.end() || keys->second.empty()) {
            if (out_error)
                *out_error = AccessToken::ParseError::unknown_app_id;
            return util::none;
        }

        for (const PKey& key : keys->second) {
            AccessToken verified_token;
            SingleKeyVerifier verifier{key};
            if (AccessToken::parse(signed_token, verified_token, error, &verifier)) {
                if (out_error)
                    *out_error = AccessToken::ParseError::none;
                return verified_token;
            }
        }
        if (out_error)
            *out_error = AccessToken::ParseError::invalid_signature;
        return util::none;
    }

    // For the purpose of testing, public key is allowed to be absent. When it
    // is absent, we set `out_error` to
    // `AccessToken::ParseError::invalid_signature` but still pass the parsed
    // token back to the caller.
    AccessToken::Verifier* verifier = nullptr;
    if (BARQ_LIKELY(m_impl->m_public_key))
        verifier = &*m_impl;
    if (BARQ_LIKELY(AccessToken::parse(signed_token, token, error, verifier))) {
        if (BARQ_LIKELY(out_error)) {
            if (BARQ_LIKELY(m_impl->m_public_key)) {
                *out_error = AccessToken::ParseError::none;
            }
            else {
                *out_error = AccessToken::ParseError::invalid_signature;
            }
        }
        return token;
    }
    if (out_error)
        *out_error = error;
    return util::none;
}

bool AccessControl::can(const AccessToken& token, Privilege permission,
                        const BarqFileIdent& barq_file) const noexcept
{
    if (!token_path_matches(token, barq_file)) {
        return false;
    }
    unsigned int p = static_cast<unsigned int>(permission);
    return (token.access & p) == p;
}

bool AccessControl::can(const AccessToken& token, unsigned int mask, const BarqFileIdent& barq_file) const noexcept
{
    if (!token_path_matches(token, barq_file)) {
        return false;
    }
    return (token.access & mask) == mask;
}

AccessToken::Verifier& AccessControl::verifier() const noexcept
{
    return *m_impl;
}

bool AccessControl::uses_tenant_public_keys() const noexcept
{
    return bool(m_impl->m_tenant_public_keys);
}

// This is_admin() function is more complicated than it should be due to
// the current format of the tokens and behavior of ROS.
// This function can be simplified with new a token format.
bool AccessControl::is_admin(const AccessToken& token) const noexcept
{
    if (token.admin_field)
        return token.admin;

    if (!token.path)
        return true;

    // This will catch admins due to the way ROS makes access tokens.
    // It is not safe since it might be too liberal. This function will be
    // replaced as described above.
    if (token.access & (Privilege::ModifySchema | Privilege::SetPermissions))
        return true;

    return false;
}
