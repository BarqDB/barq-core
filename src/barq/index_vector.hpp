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
#include <unordered_set>
#include <vector>

namespace barq {

class Table;

/// Distance metric for a vector index.
enum class VectorMetric : uint8_t {
    InnerProduct = 0, // dot product (higher = closer); embeddings pre-normalized upstream
    L2 = 1,           // squared euclidean distance (lower = closer)
    Cosine = 2,       // inner product on vectors normalized at insert/query time
};

/// Build/search parameters for a vector index. Persisted with the index, so a
/// reopened index keeps the metric and graph shape it was built with.
struct VectorIndexConfig {
    VectorMetric metric = VectorMetric::InnerProduct;
    size_t m = 16;                // HNSW out-degree (graph connectivity)
    size_t ef_construction = 200; // build-time beam width (higher = better graph, slower build)
    size_t ef_search = 64;        // query-time beam width floor (higher = better recall, slower query)
};

/// A persisted approximate-nearest-neighbour (HNSW) index over a list-of-floats
/// property. The graph is stored natively in copy-on-write arrays hanging off the
/// per-column search-index ref slot of the owning table: a header, the id->key map,
/// per-node levels, stride-addressed adjacency lists, and the vector store. Nothing
/// is loaded eagerly — reads page directly from the memory-mapped file, and a write
/// touches only the arrays it changes. Snapshot isolation (MVCC) follows from the
/// group's copy-on-write semantics: readers keep the subtree their transaction saw.
///
/// Maintenance is lazy: the graph absorbs data changes (new/removed objects) the
/// next time it is searched inside a write transaction, or on rebuild(). Searches
/// in read transactions stay correct in the meantime by brute-forcing the not yet
/// absorbed keys and merging them into the result.
///
/// The index is a derived, local structure — never written to sync changesets.
class VectorIndex {
public:
    // Create a fresh (empty) vector index.
    VectorIndex(ColKey column, Allocator& alloc, const VectorIndexConfig& config = {});
    // Reattach to a persisted index (config is read back from the persisted header).
    VectorIndex(ref_type ref, ArrayParent* parent, size_t ndx_in_parent, ColKey column, Allocator& alloc);
    ~VectorIndex();

    const VectorIndexConfig& config() const noexcept
    {
        return m_config;
    }

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

    // Record that `key`'s vector was edited in place (write transactions only —
    // called from the list-of-floats write path). The key joins the persisted
    // pending list; searches re-rank it from live data until the graph absorbs it.
    void mark_dirty(ObjKey key);

    // Query the index. Returns up to `k` object keys closest to `query`, closest
    // first, restricted to `candidates` when given (pass nullptr for an unfiltered
    // search — cheaper: no candidate set to build; results are validated against
    // the table instead). In a write transaction this first absorbs any unindexed
    // data changes into the graph. `ef` overrides the configured search beam for
    // this query (0 = use config).
    std::vector<ObjKey> search(const Table& table, const std::vector<float>& query, size_t k,
                               const std::unordered_set<uint64_t>* candidates, size_t ef = 0);
    std::vector<ObjKey> search(const Table& table, const std::vector<float>& query, size_t k,
                               const std::unordered_set<uint64_t>& candidates, size_t ef = 0)
    {
        return search(table, query, k, &candidates, ef);
    }

    // Metadata read from the persisted header.
    size_t dim() const;
    size_t count() const; // live (non-tombstoned) elements

    struct Trees; // PIMPL: accessors for the persisted graph arrays
    struct Cache; // PIMPL: in-memory lookaside (key->id map, overlay, scratch)

private:
    void create_persisted_state(Allocator& alloc);
    void attach_trees();
    void reset_caches();
    void load_config_from_header();
    uint64_t header_field(size_t index) const;

    // Maintenance (write transactions only):
    void absorb(const Table& table);   // fold outstanding data changes into the graph
    void do_rebuild(const Table& table);
    void insert_element(int64_t key, const std::vector<float>& vec);
    void tombstone(int64_t id);

    void ensure_synced(const Table& table); // absorb (writable) or compute the overlay (read-only)
    void ensure_key_map();

    ColKey m_column;
    Array m_top;
    VectorIndexConfig m_config;
    std::unique_ptr<Trees> m_trees;
    std::unique_ptr<Cache> m_cache;
};

} // namespace barq

#endif // BARQ_INDEX_VECTOR_HPP
