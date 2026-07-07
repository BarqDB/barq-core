#include <barq/sync/noinst/server/server_file_access_cache.hpp>
#include <barq/util/sha_crypto.hpp>

#include <cstdint>
#include <cstring>

using namespace barq;
using namespace _impl;

namespace {

std::array<uint8_t, 32> hkdf_extract(const std::vector<char>& master_secret)
{
    if (master_secret.empty())
        throw util::runtime_error("Tenant encryption master secret must not be empty");

    static constexpr std::array<uint8_t, 32> salt = {
        'b', 'a', 'r', 'q', ' ', 's', 'y', 'n', 'c', ' ', 't', 'e', 'n', 'a', 'n', 't',
        ' ', 'h', 'k', 'd', 'f', ' ', 'v', '1', 0,   0,   0,   0,   0,   0,   0,   0,
    };

    std::array<uint8_t, 32> prk;
    util::hmac_sha256(util::Span<const uint8_t>{reinterpret_cast<const uint8_t*>(master_secret.data()),
                                                master_secret.size()},
                      util::Span<uint8_t, 32>{prk},
                      util::Span<const uint8_t, 32>{salt}); // Throws
    return prk;
}

std::vector<uint8_t> make_hkdf_expand_input(util::Span<const uint8_t> previous_block, const std::string& tenant_id,
                                            uint8_t block_index)
{
    static constexpr const char info_prefix[] = "barq sync server tenant db encryption key v1:";
    std::vector<uint8_t> input;
    input.reserve(previous_block.size() + sizeof(info_prefix) - 1 + tenant_id.size() + 1); // Throws
    input.insert(input.end(), previous_block.begin(), previous_block.end());               // Throws
    input.insert(input.end(), info_prefix, info_prefix + sizeof(info_prefix) - 1);         // Throws
    input.insert(input.end(), tenant_id.begin(), tenant_id.end());                         // Throws
    input.push_back(block_index);                                                         // Throws
    return input;
}

std::array<uint8_t, 32> hmac_block(const std::vector<uint8_t>& input, const std::array<uint8_t, 32>& prk)
{
    std::array<uint8_t, 32> block;
    util::hmac_sha256(util::Span<const uint8_t>{input.data(), input.size()}, util::Span<uint8_t, 32>{block},
                      util::Span<const uint8_t, 32>{prk}); // Throws
    return block;
}

} // unnamed namespace

std::array<char, 64> _impl::derive_tenant_encryption_key(const std::string& tenant_id,
                                                         const std::vector<char>& master_secret)
{
    auto prk = hkdf_extract(master_secret); // Throws

    auto block_1_input = make_hkdf_expand_input(util::Span<const uint8_t>{}, tenant_id, 1);    // Throws
    auto block_1 = hmac_block(block_1_input, prk);                                            // Throws
    auto block_2_input = make_hkdf_expand_input(util::Span<const uint8_t>{block_1}, tenant_id, 2); // Throws
    auto block_2 = hmac_block(block_2_input, prk);                                            // Throws

    std::array<char, 64> key;
    std::memcpy(key.data(), block_1.data(), block_1.size());
    std::memcpy(key.data() + block_1.size(), block_2.data(), block_2.size());
    return key;
}


void ServerFileAccessCache::proper_close_all()
{
    while (m_first_open_file)
        m_first_open_file->proper_close(); // Throws
}

void ServerFileAccessCache::close_least_recently_accessed_for_tenant(const std::string& tenant_id)
{
    if (!m_first_open_file)
        return;

    Slot* slot = m_first_open_file->m_prev_open_file;
    for (;;) {
        Slot* previous = slot->m_prev_open_file;
        if (slot->tenant_id == tenant_id) {
            slot->proper_close(); // Throws
            return;
        }
        if (slot == m_first_open_file)
            break;
        slot = previous;
    }
}


void ServerFileAccessCache::access(Slot& slot)
{
    if (slot.is_open()) {
        m_logger.trace(util::LogCategory::server, "Using already open Barq file: %1", slot.barq_path); // Throws

        // Move to front
        BARQ_ASSERT(m_first_open_file);
        if (&slot != m_first_open_file) {
            remove(slot);
            insert(slot); // At front
            m_first_open_file = &slot;
        }
        return;
    }

    // Close least recently accessed Barq file
    if (m_num_open_files == m_max_open_files) {
        BARQ_ASSERT(m_first_open_file);
        Slot& least_recently_accessed = *m_first_open_file->m_prev_open_file;
        least_recently_accessed.proper_close(); // Throws
    }

    if (m_max_open_files_per_tenant) {
        auto i = m_num_open_files_by_tenant.find(slot.tenant_id);
        if (i != m_num_open_files_by_tenant.end() &&
            std::size_t(i->second) >= m_max_open_files_per_tenant) {
            close_least_recently_accessed_for_tenant(slot.tenant_id); // Throws
        }
    }

    slot.open(); // Throws
}

void ServerFileAccessCache::Slot::proper_close()
{
    if (is_open()) {
        m_cache.m_logger.detail("Closing Barq file: %1", barq_path); // Throws
        do_close();
    }
}


void ServerFileAccessCache::Slot::open()
{
    BARQ_ASSERT(!is_open());

    m_cache.m_logger.detail("Opening Barq file: %1", barq_path); // Throws

    m_file.reset(new File{*this}); // Throws

    m_cache.insert(*this);
    m_cache.m_first_open_file = this;
    ++m_cache.m_num_open_files;
    ++m_cache.m_num_open_files_by_tenant[tenant_id];
}
