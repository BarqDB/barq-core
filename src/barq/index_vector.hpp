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

#include <barq/array.hpp>
#include <barq/keys.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace barq {

class Table;

/// A persisted approximate-nearest-neighbour (HNSW) index over a list-of-floats
/// property. Unlike the StringIndex family this is a standalone accessor: it
/// stores the serialized hnswlib graph as a chunked blob hung off a per-column
/// slot in the table top array, and keeps a lazily (re)built in-memory copy of
/// the graph for querying.
///
/// The graph is a derived, local structure — it is never written to sync
/// changesets. On open the persisted blob is loaded (no rebuild) as long as the
/// table's content version still matches the version the graph was built at; if
/// the data changed since, the graph is rebuilt in memory on demand and
/// re-persisted on the next explicit rebuild().
class VectorIndex {
public:
    // Create a fresh (empty) vector index container.
    VectorIndex(ColKey column, Allocator& alloc);
    // Reattach to a persisted container.
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
    // to `candidates` (the keys that survived any preceding query), closest first.
    // Rebuilds the graph in memory if it is stale — this keeps reads correct without
    // requiring a write transaction (the rebuilt graph is not persisted here).
    std::vector<ObjKey> search(const Table& table, const std::vector<float>& query, size_t k,
                               const std::unordered_set<uint64_t>& candidates);

    // Metadata (read from the persisted container).
    size_t dim() const;
    size_t count() const;           // number of vectors in the persisted graph
    size_t stored_row_count() const; // table row count when the graph was built

private:
    struct Graph; // PIMPL: holds the hnswlib space + graph, keeping hnswlib out of this header.

    void ensure_graph(const Table& table);
    void build_in_memory(const Table& table);
    bool load_from_blob();
    void store_to_blob(size_t row_count);
    void read_all_bytes(std::string& out) const;

    ColKey m_column;
    Array m_top; // container root: slot 0 = meta array, slot 1 = chunk refs
    std::unique_ptr<Graph> m_graph;
    uint64_t m_graph_version = uint64_t(-1); // in-session cache validity (allocator content version)
    size_t m_graph_dim = 0;
    bool m_tried_load = false; // whether we already attempted to load the persisted blob this session
};

} // namespace barq

#endif // BARQ_INDEX_VECTOR_HPP
