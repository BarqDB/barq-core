/*************************************************************************
 *
 * Copyright 2024 Realm Inc.
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

#ifndef BARQ_INDEX_VECTOR_HPP
#define BARQ_INDEX_VECTOR_HPP

#include <barq/array_blob.hpp>
#include <barq/keys.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace barq {

class Table;

/// A persisted approximate-nearest-neighbour (HNSW) index over a list-of-floats
/// property. It is a standalone accessor whose single on-disk node is an ArrayBlob
/// holding a small fixed header (dim, element count, table row count at build time)
/// followed by the serialized hnswlib graph. The blob ref lives in the per-column
/// search-index ref slot of the owning table.
///
/// The graph is a derived, local structure — never written to sync changesets. On
/// open the persisted blob is loaded (no rebuild); if the table's row count changed
/// since it was built, the in-memory graph is reconciled incrementally.
class VectorIndex {
public:
    // Create a fresh (empty) vector index.
    VectorIndex(ColKey column, Allocator& alloc);
    // Reattach to a persisted blob.
    VectorIndex(ref_type ref, ArrayParent* parent, size_t ndx_in_parent, ColKey column, Allocator& alloc);
    ~VectorIndex();

    // Accessor concept (mirrors SearchIndex):
    Allocator& get_alloc() const noexcept;
    void destroy() noexcept;
    bool is_attached() const noexcept;
    void set_parent(ArrayParent* parent, size_t ndx_in_parent) noexcept;
    size_t get_ndx_in_parent() const noexcept;
    void update_from_parent() noexcept;
    void refresh_accessor_tree();
    ref_type get_ref() const noexcept;

    ColKey get_column_key() const noexcept
    {
        return m_column;
    }

    // Build the graph from `table` and persist it. Must run in a write transaction.
    void rebuild(const Table& table);

    // Query the index. Returns up to `k` object keys closest to `query`, restricted
    // to `candidates`, closest first.
    std::vector<ObjKey> search(const Table& table, const std::vector<float>& query, size_t k,
                               const std::unordered_set<uint64_t>& candidates);

    // Metadata read from the persisted blob header.
    size_t dim() const;
    size_t count() const;
    size_t stored_row_count() const;

private:
    struct Graph; // PIMPL: holds the hnswlib space + graph, keeping hnswlib out of this header.

    void ensure_graph(const Table& table);
    void build_in_memory(const Table& table);
    bool sync_in_memory(const Table& table); // incremental add/delete; true if the graph changed
    bool load_from_blob();
    void store_to_blob(size_t row_count);
    uint64_t header_field(size_t index) const; // read a header field, 0 if not present

    ColKey m_column;
    ArrayBlob m_blob; // single on-disk node: [header][serialized hnswlib graph]
    std::unique_ptr<Graph> m_graph;
    uint64_t m_graph_version = uint64_t(-1);
    size_t m_graph_dim = 0;
    bool m_tried_load = false;
};

} // namespace barq

#endif // BARQ_INDEX_VECTOR_HPP
