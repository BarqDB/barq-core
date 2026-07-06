#include <barq/sync/noinst/server/server_file_access_cache.hpp>

using namespace barq;
using namespace _impl;


void ServerFileAccessCache::proper_close_all()
{
    while (m_first_open_file)
        m_first_open_file->proper_close(); // Throws
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
}
