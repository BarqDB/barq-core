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

/// How the index stores its copy of the vectors.
enum class VectorEncoding : uint8_t {
    Float32 = 0, // full precision, 4 bytes per dimension
    SQ8 = 1,     // scalar-quantized to 1 byte per dimension (per-dimension linear scale,
                 // learned at build time). ~4x smaller and less build RAM; searches walk
                 // the graph on quantized distances and re-rank the top candidates
                 // exactly against the table data, so recall stays near full precision.
};

/// The admissible keys of a filtered vector search: a dense bitmap over the key
/// range when the keys pack tightly (the common case — ObjKeys allocate
/// sequentially), a hash set otherwise. Building and probing the bitmap is
/// several times cheaper than a hash set of the same keys.
class VectorCandidates {
public:
    explicit VectorCandidates(std::vector<uint64_t> keys);

    bool contains(uint64_t key) const noexcept
    {
        if (m_dense) {
            if (key < m_min || key > m_max)
                return false;
            uint64_t bit = key - m_min;
            return (m_bits[size_t(bit >> 6)] >> (bit & 63)) & 1;
        }
        return m_sparse.count(key) != 0;
    }
    size_t size() const noexcept
    {
        return m_count;
    }
    // Visit every key. Used by exact candidate scans; the dense walk is a
    // per-bit scan.
    template <typename Fn>
    void for_each(Fn&& fn) const
    {
        if (!m_dense) {
            for (uint64_t key : m_sparse)
                fn(key);
            return;
        }
        for (size_t w = 0; w < m_bits.size(); ++w) {
            uint64_t bits = m_bits[w];
            for (unsigned b = 0; bits; ++b, bits >>= 1) {
                if (bits & 1)
                    fn(m_min + (uint64_t(w) << 6) + b);
            }
        }
    }

private:
    std::vector<uint64_t> m_bits;      // dense: bit (key - m_min)
    std::unordered_set<uint64_t> m_sparse;
    uint64_t m_min = 0, m_max = 0;
    size_t m_count = 0;
    bool m_dense = false;
};

/// Build/search parameters for a vector index. Persisted with the index (except
/// build_threads), so a reopened index keeps the metric and graph shape it was
/// built with.
struct VectorIndexConfig {
    VectorMetric metric = VectorMetric::InnerProduct;
    VectorEncoding encoding = VectorEncoding::Float32; // vector storage; fixed at creation
    size_t dimensions = 0;        // declared vector length. 0 = infer from the first vector
                                  // (legacy: other sizes are silently skipped). When > 0 the
                                  // dimension is enforced — a stored or query vector of any
                                  // other non-empty length is rejected, not dropped.
    size_t m = 16;                // HNSW out-degree (graph connectivity)
    size_t ef_construction = 200; // build-time beam width (higher = better graph, slower build)
    size_t ef_search = 0;         // query-time beam width floor (higher = better recall, slower query).
                                  // 0 = auto: widens with index size (64 up to 100k vectors, 128 up to
                                  // 1M, 192 up to 10M, 256 beyond) so recall holds steady as data grows.
    size_t build_threads = 0;     // worker threads for full (re)builds; 0 = one per core. Not persisted.
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
/// Data changes are tracked as they happen: the table notifies the index of every
/// object insert/erase (object_inserted/object_erased) and of every in-place vector
/// edit (mark_dirty), and the keys queue up in small persisted event lists. Both
/// absorbing and the read-only overlay consume those lists, so syncing costs
/// O(changes), never a table scan. Legacy (pre-tracking) indexes fall back to a
/// one-off full-table diff and upgrade to event tracking on their next absorb.
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

    // Adopt a new query-time beam floor (write transactions only). ef_search
    // shapes searches, not the persisted graph, so the header is patched in
    // place and no rebuild happens.
    void set_ef_search(size_t ef_search);

    // Change notifications from the owning table (write transactions only).
    // The keys queue up in persisted event lists consumed by the next absorb;
    // read transactions answer for them via the overlay. No-ops on a legacy
    // (pre-tracking) index, which reconciles by table diff instead.
    void object_inserted(ObjKey key); // called for every object created
    void object_erased(ObjKey key);   // called for every object removed
    void table_cleared();             // all objects removed at once: empty the graph

    // Query the index. Returns up to `k` object keys closest to `query`, closest
    // first, restricted to `candidates` when given (pass nullptr for an unfiltered
    // search — cheaper: no candidate set to build; results are validated against
    // the table instead). A tiny candidate set skips the graph entirely and is
    // ranked exactly from live table data. In a write transaction a graph search
    // first absorbs any unindexed data changes. `ef` overrides the configured
    // search beam for this query (0 = use config). An explicit `ef` at least as
    // large as the live candidate count performs a flat exact scan.
    std::vector<ObjKey> search(const Table& table, const std::vector<float>& query, size_t k,
                               const VectorCandidates* candidates, size_t ef = 0);
    std::vector<ObjKey> search(const Table& table, const std::vector<float>& query, size_t k,
                               const VectorCandidates& candidates, size_t ef = 0)
    {
        return search(table, query, k, &candidates, ef);
    }

    // Metadata read from the persisted header.
    size_t dim() const;
    size_t count() const; // live (non-tombstoned) elements

    // Strip the event lists and downgrade the persisted format to the legacy
    // (pre-tracking) layout. Exercises the lazy upgrade path in tests.
    void _downgrade_to_legacy_format_for_testing();
    // Override the event-list cap (0 = default). Exercises the overflow path in tests.
    static size_t _max_events_for_testing;

    struct Trees; // PIMPL: accessors for the persisted graph arrays
    struct Cache; // PIMPL: in-memory lookaside (key->id map, overlay, scratch)

private:
    void create_persisted_state(Allocator& alloc);
    void attach_trees();
    void reset_caches();
    void load_config_from_header();
    // Re-read m_top from its parent slot (and re-attach the trees) when the two
    // have diverged — which happens after a write that created/changed this index
    // commits, or when a sibling search index modifies the shared m_index_refs
    // array. Callers must hold m_cache->mutex.
    void refresh_if_stale();
    uint64_t header_field(size_t index) const;

    // Maintenance (write transactions only). Full builds (do_rebuild, and absorb
    // when the graph is empty or the unindexed delta dominates it) assemble the
    // graph in flat RAM on all cores and persist it in one sequential pass;
    // small deltas go through one-at-a-time inserts into the persisted arrays.
    void absorb(const Table& table);       // fold outstanding data changes into the graph
    void event_absorb(const Table& table); // consume the event lists: O(changes)
    void scan_absorb(const Table& table);  // legacy/overflow fallback: diff by table scan
    void do_rebuild(const Table& table);
    void insert_element(int64_t key, const float* vec, size_t dim);
    void tombstone(int64_t id);

    // Event-tracking plumbing.
    bool tracked() const noexcept
    {
        return m_tracked;
    }
    bool events_overflowed() const;         // recording gave up; next absorb diffs by scan
    void make_tracked();                    // upgrade a just-reconciled index to event tracking
    void record_event(size_t slot, ObjKey key);
    void verify_matches_table(const Table& table); // debug-only cross-check after an absorb

    void ensure_synced(const Table& table); // absorb (writable) or compute the overlay (read-only)
    void ensure_key_map();

    ColKey m_column;
    Array m_top;
    VectorIndexConfig m_config;
    std::unique_ptr<Trees> m_trees;
    std::unique_ptr<Cache> m_cache;
    bool m_tracked = false; // persisted layout has event lists (set by attach_trees)
};

} // namespace barq

#endif // BARQ_INDEX_VECTOR_HPP
