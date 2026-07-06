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

#ifndef BARQ_COLUMN_BINARY_HPP
#define BARQ_COLUMN_BINARY_HPP

#include <barq/bplustree.hpp>
#include <barq/array_binary.hpp>
#include <barq/array_blobs_small.hpp>
#include <barq/array_blobs_big.hpp>

namespace barq {

class BinaryColumn : public BPlusTree<BinaryData> {
public:
    using BPlusTree::BPlusTree;
    BinaryData get_at(size_t ndx, size_t& pos) const noexcept;
};

class BinaryIterator {
public:
    BinaryIterator() {}

    BinaryIterator(BinaryData binary)
        : m_binary(binary)
    {
    }

    BinaryIterator(const BinaryColumn* col, size_t ndx)
        : m_binary_col(col)
        , m_ndx(ndx)
    {
        BARQ_ASSERT(col);
    }

    BinaryData get_next() noexcept
    {
        if (!m_end_of_data) {
            if (m_binary_col) {
                BinaryData ret = m_binary_col->get_at(m_ndx, m_pos);
                m_end_of_data = (m_pos == 0);
                return ret;
            }
            else if (!m_binary.is_null()) {
                m_end_of_data = true;
                return m_binary;
            }
        }
        return {};
    }

    BinaryData get_only() const noexcept
    {
        BARQ_ASSERT(m_binary);
        return m_binary;
    }

private:
    bool m_end_of_data = false;
    const BinaryColumn* m_binary_col = nullptr;
    size_t m_ndx = 0;
    size_t m_pos = 0;
    BinaryData m_binary;
};

} // namespace barq

#endif // BARQ_COLUMN_BINARY_HPP
