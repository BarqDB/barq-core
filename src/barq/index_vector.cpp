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

#include <barq/index_vector.hpp>

#include <barq/bplustree.hpp>
#include <barq/list.hpp>
#include <barq/table.hpp>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <queue>
#include <random>
#include <unordered_map>

// The HNSW insert/search algorithms below are ported from hnswlib (vendored under
// src/external/hnswlib, Apache 2.0) to operate directly on barq's copy-on-write
// arrays instead of hnswlib's flat in-process memory.

using namespace barq;

namespace {

// ---- persisted layout ----------------------------------------------------------
//
// m_top (HasRefs):
//   [0] header       Array of ints (layout below)
//   [1] keys         BPlusTree<int64>  id -> ObjKey value, -1 = tombstone
//   [2] levels       BPlusTree<int64>  id -> top layer of the node
//   [3] links0       BPlusTree<int64>  layer-0 adjacency, stride 2m+1: [count, n...]
//   [4] upper_ofs    BPlusTree<int64>  id -> first upper block index, -1 if level 0
//   [5] links_upper  BPlusTree<int64>  upper adjacency, stride m+1 per (id, layer)
//   [6] vectors      BPlusTree<float>  id -> the vector, stride dim
//   [7] pending      BPlusTree<int64>  ObjKeys edited in place, absorbed lazily

constexpr size_t t_header = 0;
constexpr size_t t_keys = 1;
constexpr size_t t_levels = 2;
constexpr size_t t_links0 = 3;
constexpr size_t t_upper_ofs = 4;
constexpr size_t t_links_upper = 5;
constexpr size_t t_vectors = 6;
constexpr size_t t_pending = 7;
constexpr size_t t_count = 8;

constexpr size_t h_format = 0;
constexpr size_t h_dim = 1;
constexpr size_t h_metric = 2;
constexpr size_t h_m = 3;
constexpr size_t h_efc = 4;
constexpr size_t h_efs = 5;
constexpr size_t h_entry = 6;    // id + 1, 0 = none
constexpr size_t h_maxlevel = 7; // level + 1, 0 = none
constexpr size_t h_total = 8;    // ids allocated, including tombstones
constexpr size_t h_deleted = 9;
constexpr size_t h_salt = 10;
constexpr size_t h_count = 11;

constexpr int64_t s_format = 3; // native array-backed graph

// ---- small helpers -------------------------------------------------------------

uint64_t splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

// Normalize in place (cosine metric); zero vectors are left untouched.
void normalize_vec(float* v, size_t dim)
{
    double n = 0;
    for (size_t i = 0; i < dim; ++i)
        n += double(v[i]) * double(v[i]);
    if (n > 0) {
        float inv = float(1.0 / std::sqrt(n));
        for (size_t i = 0; i < dim; ++i)
            v[i] *= inv;
    }
}

// Distance where smaller = closer (hnswlib convention).
float metric_dist(VectorMetric metric, const float* a, const float* b, size_t dim)
{
    if (metric == VectorMetric::L2) {
        double s = 0;
        for (size_t i = 0; i < dim; ++i) {
            double d = double(a[i]) - double(b[i]);
            s += d * d;
        }
        return float(s);
    }
    // Inner product / cosine (vectors pre-normalized for cosine).
    double dot = 0;
    for (size_t i = 0; i < dim; ++i)
        dot += double(a[i]) * double(b[i]);
    return float(1.0 - dot);
}

// Global lock for the shared scratch-free engine is NOT needed: every accessor works
// on its own snapshot arrays and its own caches; the per-accessor cache mutex below
// covers a frozen table shared across threads.

} // anonymous namespace

// ---- accessors for the persisted graph -----------------------------------------

struct VectorIndex::Trees {
    explicit Trees(Allocator& alloc)
        : header(alloc)
        , keys(alloc)
        , levels(alloc)
        , links0(alloc)
        , upper_ofs(alloc)
        , links_upper(alloc)
        , vectors(alloc)
        , pending(alloc)
    {
    }

    Array header;
    BPlusTree<int64_t> keys;
    BPlusTree<int64_t> levels;
    BPlusTree<int64_t> links0;
    BPlusTree<int64_t> upper_ofs;
    BPlusTree<int64_t> links_upper;
    BPlusTree<float> vectors;
    BPlusTree<int64_t> pending;

    int64_t hdr(size_t field) const
    {
        return header.get(field);
    }
    void set_hdr(size_t field, int64_t value)
    {
        header.set(field, value);
    }
};

// ---- in-memory lookaside --------------------------------------------------------

struct VectorIndex::Cache {
    std::unordered_map<int64_t, int64_t> key2id; // live keys only
    bool key2id_valid = false;

    // Read-only transactions: keys the graph has not absorbed yet (new objects and
    // pending in-place edits) get brute-forced and merged into the result.
    std::vector<int64_t> overlay;
    std::unordered_set<int64_t> stale; // keys whose graph copy is outdated (pending edits)
    uint64_t synced_version = uint64_t(-1);

    // Keys already recorded in the persisted pending list — keeps per-element list
    // writes (one touch per lst.add) from rescanning it.
    std::unordered_set<int64_t> pending_seen;

    // A frozen table (and thus this accessor) may be shared across threads.
    std::mutex mutex;

    void reset()
    {
        key2id.clear();
        key2id_valid = false;
        overlay.clear();
        stale.clear();
        synced_version = uint64_t(-1);
        pending_seen.clear();
    }
};

// ---- graph engine ---------------------------------------------------------------

namespace {

// The HNSW algorithms, operating on the persisted trees. Reads page straight from
// the memory-mapped arrays (with the B+-tree leaf cache keeping repeated hits warm);
// writes copy-on-write only the leaves they touch.
struct GraphOps {
    VectorIndex::Trees& t;
    const VectorIndexConfig& cfg;
    VectorMetric metric;
    size_t dim;

    size_t stride0() const
    {
        return 2 * cfg.m + 1;
    }
    size_t stride_up() const
    {
        return cfg.m + 1;
    }

    int64_t entry() const
    {
        return t.hdr(h_entry) - 1;
    }
    int max_level() const
    {
        return int(t.hdr(h_maxlevel)) - 1;
    }
    int64_t total() const
    {
        return t.hdr(h_total);
    }

    void read_vec(int64_t id, float* out) const
    {
        size_t base = size_t(id) * dim;
        for (size_t i = 0; i < dim; ++i)
            out[i] = t.vectors.get(base + i);
    }

    float dist_to(const float* q, int64_t id, std::vector<float>& scratch) const
    {
        read_vec(id, scratch.data());
        return metric_dist(metric, q, scratch.data(), dim);
    }

    void neighbors(int64_t id, int layer, std::vector<int64_t>& out) const
    {
        out.clear();
        size_t base;
        if (layer == 0) {
            base = size_t(id) * stride0();
            int64_t cnt = t.links0.get(base);
            for (int64_t i = 0; i < cnt; ++i)
                out.push_back(t.links0.get(base + 1 + size_t(i)));
        }
        else {
            int64_t block = t.upper_ofs.get(size_t(id));
            base = size_t(block + layer - 1) * stride_up();
            int64_t cnt = t.links_upper.get(base);
            for (int64_t i = 0; i < cnt; ++i)
                out.push_back(t.links_upper.get(base + 1 + size_t(i)));
        }
    }

    void set_neighbors(int64_t id, int layer, const std::vector<int64_t>& list)
    {
        if (layer == 0) {
            size_t base = size_t(id) * stride0();
            t.links0.set(base, int64_t(list.size()));
            for (size_t i = 0; i < list.size(); ++i)
                t.links0.set(base + 1 + i, list[i]);
        }
        else {
            int64_t block = t.upper_ofs.get(size_t(id));
            size_t base = size_t(block + layer - 1) * stride_up();
            t.links_upper.set(base, int64_t(list.size()));
            for (size_t i = 0; i < list.size(); ++i)
                t.links_upper.set(base + 1 + i, list[i]);
        }
    }

    // Greedy 1-best descent within a layer (used above the target layer).
    int64_t greedy(const float* q, int64_t curr, int layer, std::vector<float>& scratch) const
    {
        float best = dist_to(q, curr, scratch);
        bool changed = true;
        std::vector<int64_t> nbrs;
        while (changed) {
            changed = false;
            neighbors(curr, layer, nbrs);
            for (int64_t n : nbrs) {
                float d = dist_to(q, n, scratch);
                if (d < best) {
                    best = d;
                    curr = n;
                    changed = true;
                }
            }
        }
        return curr;
    }

    using Hit = std::pair<float, int64_t>; // (distance, id)

    // Beam search within one layer (ported from hnswlib::searchBaseLayer).
    // `admit` decides whether a node may enter the result set; every node is still
    // traversed for connectivity. Results are returned sorted, closest first.
    template <typename Admit>
    std::vector<Hit> search_layer(const float* q, int64_t entry_point, size_t ef, int layer, Admit admit,
                                  std::vector<float>& scratch) const
    {
        std::priority_queue<Hit> results;                                 // max-heap: worst admitted on top
        std::priority_queue<Hit, std::vector<Hit>, std::greater<>> cands; // min-heap: closest first
        std::unordered_set<int64_t> visited;

        float d0 = dist_to(q, entry_point, scratch);
        cands.emplace(d0, entry_point);
        visited.insert(entry_point);
        if (admit(entry_point))
            results.emplace(d0, entry_point);

        std::vector<int64_t> nbrs;
        while (!cands.empty()) {
            Hit c = cands.top();
            if (results.size() >= ef && c.first > results.top().first)
                break;
            cands.pop();

            neighbors(c.second, layer, nbrs);
            for (int64_t n : nbrs) {
                if (!visited.insert(n).second)
                    continue;
                float d = dist_to(q, n, scratch);
                if (results.size() < ef || d < results.top().first) {
                    cands.emplace(d, n);
                    if (admit(n)) {
                        results.emplace(d, n);
                        if (results.size() > ef)
                            results.pop();
                    }
                }
            }
        }

        std::vector<Hit> out(results.size());
        for (size_t i = out.size(); i-- > 0;) {
            out[i] = results.top();
            results.pop();
        }
        return out;
    }

    // Neighbour selection heuristic (ported from hnswlib::getNeighborsByHeuristic2):
    // keep a candidate only if it is closer to the query than to every already kept
    // neighbour — spreads the links out instead of clustering them.
    std::vector<int64_t> select_neighbors(const std::vector<Hit>& cands_sorted, size_t m,
                                          std::vector<float>& scratch_a, std::vector<float>& scratch_b) const
    {
        std::vector<int64_t> result;
        if (cands_sorted.size() <= m) {
            for (auto& c : cands_sorted)
                result.push_back(c.second);
            return result;
        }
        for (auto& c : cands_sorted) {
            if (result.size() >= m)
                break;
            read_vec(c.second, scratch_a.data());
            bool good = true;
            for (int64_t r : result) {
                read_vec(r, scratch_b.data());
                if (metric_dist(metric, scratch_a.data(), scratch_b.data(), dim) < c.first) {
                    good = false;
                    break;
                }
            }
            if (good)
                result.push_back(c.second);
        }
        return result;
    }

    // Level assignment: deterministic per id (exponential distribution, as hnswlib).
    int level_for(int64_t id) const
    {
        uint64_t h = splitmix64(uint64_t(id) ^ uint64_t(t.hdr(h_salt)));
        double u = (double(h >> 11) + 1.0) / 9007199254740993.0; // (0, 1)
        double mult = 1.0 / std::log(double(cfg.m));
        return int(-std::log(u) * mult);
    }
};

} // anonymous namespace

// ---- VectorIndex ----------------------------------------------------------------

void VectorIndex::create_persisted_state(Allocator& alloc)
{
    m_top.create(Array::type_HasRefs);

    Array header(alloc);
    header.create(Array::type_Normal, false, h_count, 0);
    header.set(h_format, s_format);
    header.set(h_metric, int64_t(m_config.metric));
    header.set(h_m, int64_t(m_config.m));
    header.set(h_efc, int64_t(m_config.ef_construction));
    header.set(h_efs, int64_t(m_config.ef_search));
    std::random_device rd;
    header.set(h_salt, int64_t((uint64_t(rd()) << 32) ^ rd()));
    m_top.add(from_ref(header.get_ref()));

    for (size_t slot = t_keys; slot < t_count; ++slot) {
        if (slot == t_vectors) {
            BPlusTree<float> tree(alloc);
            tree.create();
            m_top.add(from_ref(tree.get_ref()));
        }
        else {
            BPlusTree<int64_t> tree(alloc);
            tree.create();
            m_top.add(from_ref(tree.get_ref()));
        }
    }
    attach_trees();
}

void VectorIndex::attach_trees()
{
    Allocator& alloc = m_top.get_alloc();
    if (!m_trees)
        m_trees = std::make_unique<Trees>(alloc);
    m_trees->header.set_parent(&m_top, t_header);
    m_trees->header.init_from_parent();
    m_trees->keys.set_parent(&m_top, t_keys);
    m_trees->keys.init_from_parent();
    m_trees->levels.set_parent(&m_top, t_levels);
    m_trees->levels.init_from_parent();
    m_trees->links0.set_parent(&m_top, t_links0);
    m_trees->links0.init_from_parent();
    m_trees->upper_ofs.set_parent(&m_top, t_upper_ofs);
    m_trees->upper_ofs.init_from_parent();
    m_trees->links_upper.set_parent(&m_top, t_links_upper);
    m_trees->links_upper.init_from_parent();
    m_trees->vectors.set_parent(&m_top, t_vectors);
    m_trees->vectors.init_from_parent();
    m_trees->pending.set_parent(&m_top, t_pending);
    m_trees->pending.init_from_parent();
}

void VectorIndex::reset_caches()
{
    if (m_cache)
        m_cache->reset();
}

VectorIndex::VectorIndex(ColKey column, Allocator& alloc, const VectorIndexConfig& config)
    : m_column(column)
    , m_top(alloc)
    , m_config(config)
    , m_cache(std::make_unique<Cache>())
{
    create_persisted_state(alloc);
}

VectorIndex::VectorIndex(ref_type ref, ArrayParent* parent, size_t ndx_in_parent, ColKey column, Allocator& alloc)
    : m_column(column)
    , m_top(alloc)
    , m_cache(std::make_unique<Cache>())
{
    m_top.init_from_ref(ref);
    m_top.set_parent(parent, ndx_in_parent);
    attach_trees();
    load_config_from_header();
}

VectorIndex::~VectorIndex() = default;

Allocator& VectorIndex::get_alloc() const noexcept
{
    return m_top.get_alloc();
}

void VectorIndex::destroy() noexcept
{
    m_top.destroy_deep();
}

bool VectorIndex::is_attached() const noexcept
{
    return m_top.is_attached();
}

void VectorIndex::set_parent(ArrayParent* parent, size_t ndx_in_parent) noexcept
{
    m_top.set_parent(parent, ndx_in_parent);
}

size_t VectorIndex::get_ndx_in_parent() const noexcept
{
    return m_top.get_ndx_in_parent();
}

void VectorIndex::update_from_parent() noexcept
{
    std::lock_guard<std::mutex> lock(m_cache->mutex);
    m_top.init_from_parent();
    attach_trees();
    load_config_from_header();
    reset_caches();
}

void VectorIndex::refresh_accessor_tree()
{
    std::lock_guard<std::mutex> lock(m_cache->mutex);
    m_top.init_from_parent();
    attach_trees();
    load_config_from_header();
    reset_caches();
}

ref_type VectorIndex::get_ref() const noexcept
{
    return m_top.get_ref();
}

uint64_t VectorIndex::header_field(size_t index) const
{
    if (!m_trees || !m_trees->header.is_attached())
        return 0;
    return uint64_t(m_trees->header.get(index));
}

void VectorIndex::load_config_from_header()
{
    if (header_field(h_format) != uint64_t(s_format))
        return;
    m_config.metric = VectorMetric(uint8_t(header_field(h_metric)));
    m_config.m = size_t(header_field(h_m));
    m_config.ef_construction = size_t(header_field(h_efc));
    m_config.ef_search = size_t(header_field(h_efs));
}

size_t VectorIndex::dim() const
{
    return size_t(header_field(h_dim));
}

size_t VectorIndex::count() const
{
    return size_t(header_field(h_total) - header_field(h_deleted));
}

void VectorIndex::ensure_key_map()
{
    if (m_cache->key2id_valid)
        return;
    m_cache->key2id.clear();
    size_t total = size_t(m_trees->hdr(h_total));
    for (size_t id = 0; id < total; ++id) {
        int64_t key = m_trees->keys.get(id);
        if (key >= 0)
            m_cache->key2id[key] = int64_t(id);
    }
    m_cache->key2id_valid = true;
}

// Insert one element into the graph (ported from hnswlib::addPoint). `vec` must
// already be normalized for the cosine metric. Write transaction only.
void VectorIndex::insert_element(int64_t key, const std::vector<float>& vec)
{
    Trees& t = *m_trees;
    GraphOps ops{t, m_config, m_config.metric, vec.size()};

    int64_t id = t.hdr(h_total);
    int level = ops.level_for(id);

    t.keys.add(key);
    t.levels.add(level);
    for (float x : vec)
        t.vectors.add(x);
    for (size_t i = 0; i < ops.stride0(); ++i)
        t.links0.add(0);
    if (level > 0) {
        int64_t block = int64_t(t.links_upper.size() / ops.stride_up());
        t.upper_ofs.add(block);
        for (int l = 0; l < level; ++l)
            for (size_t i = 0; i < ops.stride_up(); ++i)
                t.links_upper.add(0);
    }
    else {
        t.upper_ofs.add(-1);
    }
    t.set_hdr(h_total, id + 1);
    m_cache->key2id[key] = id;

    int64_t entry = ops.entry();
    int max_level = ops.max_level();
    if (entry < 0) {
        t.set_hdr(h_entry, id + 1);
        t.set_hdr(h_maxlevel, level + 1);
        return;
    }

    std::vector<float> scratch_a(vec.size()), scratch_b(vec.size());
    const float* q = vec.data();
    int64_t curr = entry;
    for (int lc = max_level; lc > level; --lc)
        curr = ops.greedy(q, curr, lc, scratch_a);

    auto admit_all = [](int64_t) {
        return true;
    };
    for (int lc = std::min(level, max_level); lc >= 0; --lc) {
        auto cands = ops.search_layer(q, curr, m_config.ef_construction, lc, admit_all, scratch_a);
        auto selected = ops.select_neighbors(cands, m_config.m, scratch_a, scratch_b);
        ops.set_neighbors(id, lc, selected);

        size_t max_m = (lc == 0) ? 2 * m_config.m : m_config.m;
        std::vector<int64_t> nbrs;
        std::vector<float> nvec(vec.size());
        for (int64_t n : selected) {
            ops.neighbors(n, lc, nbrs);
            if (nbrs.size() < max_m) {
                nbrs.push_back(id);
                ops.set_neighbors(n, lc, nbrs);
            }
            else {
                // Overflow: re-select the neighbour's links with the heuristic,
                // considering the new element too (as hnswlib does).
                ops.read_vec(n, nvec.data());
                std::vector<GraphOps::Hit> merged;
                merged.reserve(nbrs.size() + 1);
                ops.read_vec(id, scratch_a.data());
                merged.emplace_back(metric_dist(m_config.metric, nvec.data(), scratch_a.data(), vec.size()), id);
                for (int64_t x : nbrs) {
                    ops.read_vec(x, scratch_a.data());
                    merged.emplace_back(metric_dist(m_config.metric, nvec.data(), scratch_a.data(), vec.size()),
                                        x);
                }
                std::sort(merged.begin(), merged.end());
                auto keep = ops.select_neighbors(merged, max_m, scratch_a, scratch_b);
                ops.set_neighbors(n, lc, keep);
            }
        }
        if (!selected.empty())
            curr = selected.front();
    }

    if (level > max_level) {
        t.set_hdr(h_entry, id + 1);
        t.set_hdr(h_maxlevel, level + 1);
    }
}

void VectorIndex::tombstone(int64_t id)
{
    m_trees->keys.set(size_t(id), -1);
    m_trees->set_hdr(h_deleted, m_trees->hdr(h_deleted) + 1);
}

// Fold outstanding data changes into the graph: index new objects, tombstone gone
// ones, re-index pending in-place edits. Write transaction only.
void VectorIndex::absorb(const Table& table)
{
    Trees& t = *m_trees;
    ensure_key_map();

    size_t d = dim();
    if (d == 0) {
        // Dimension is discovered from the first non-empty vector.
        for (auto obj : table) {
            auto lst = obj.get_list<float>(m_column);
            if (lst.size() > 0) {
                d = lst.size();
                break;
            }
        }
        if (d == 0) {
            t.pending.clear();
            return;
        }
        t.set_hdr(h_dim, int64_t(d));
    }

    // Pending in-place edits: tombstone the stale copy; the add pass below re-inserts.
    size_t n_pending = t.pending.size();
    for (size_t i = 0; i < n_pending; ++i) {
        int64_t key = t.pending.get(i);
        auto it = m_cache->key2id.find(key);
        if (it != m_cache->key2id.end()) {
            tombstone(it->second);
            m_cache->key2id.erase(it);
        }
    }
    if (n_pending)
        t.pending.clear();

    std::vector<float> buf(d);
    std::unordered_set<int64_t> present;
    for (auto obj : table) {
        auto lst = obj.get_list<float>(m_column);
        if (lst.size() != d)
            continue;
        int64_t key = obj.get_key().value;
        present.insert(key);
        if (m_cache->key2id.count(key))
            continue;
        for (size_t i = 0; i < d; ++i)
            buf[i] = lst.get(i);
        if (m_config.metric == VectorMetric::Cosine)
            normalize_vec(buf.data(), d);
        insert_element(key, buf);
    }

    std::vector<int64_t> gone;
    for (auto& kv : m_cache->key2id) {
        if (!present.count(kv.first))
            gone.push_back(kv.first);
    }
    for (int64_t key : gone) {
        tombstone(m_cache->key2id[key]);
        m_cache->key2id.erase(key);
    }

    // Compact once tombstones dominate: rebuild from live data.
    int64_t deleted = t.hdr(h_deleted);
    if (deleted > 0 && deleted * 2 >= t.hdr(h_total))
        do_rebuild(table);
}

void VectorIndex::do_rebuild(const Table& table)
{
    Trees& t = *m_trees;
    t.keys.clear();
    t.levels.clear();
    t.links0.clear();
    t.upper_ofs.clear();
    t.links_upper.clear();
    t.vectors.clear();
    t.pending.clear();
    t.set_hdr(h_entry, 0);
    t.set_hdr(h_maxlevel, 0);
    t.set_hdr(h_total, 0);
    t.set_hdr(h_deleted, 0);
    t.set_hdr(h_dim, 0);
    m_cache->key2id.clear();
    m_cache->key2id_valid = true;

    size_t d = 0;
    for (auto obj : table) {
        auto lst = obj.get_list<float>(m_column);
        if (lst.size() > 0) {
            d = lst.size();
            break;
        }
    }
    if (d == 0)
        return;
    t.set_hdr(h_dim, int64_t(d));

    std::vector<float> buf(d);
    for (auto obj : table) {
        auto lst = obj.get_list<float>(m_column);
        if (lst.size() != d)
            continue;
        for (size_t i = 0; i < d; ++i)
            buf[i] = lst.get(i);
        if (m_config.metric == VectorMetric::Cosine)
            normalize_vec(buf.data(), d);
        insert_element(obj.get_key().value, buf);
    }
}

void VectorIndex::rebuild(const Table& table)
{
    std::lock_guard<std::mutex> lock(m_cache->mutex);
    do_rebuild(table);
    m_cache->overlay.clear();
    m_cache->stale.clear();
    m_cache->pending_seen.clear();
    m_cache->synced_version = table.get_content_version();
}

void VectorIndex::mark_dirty(ObjKey key)
{
    std::lock_guard<std::mutex> lock(m_cache->mutex);
    int64_t k = key.value;
    if (m_cache->pending_seen.count(k))
        return;
    // Dedupe against entries persisted by earlier transactions (the list is small;
    // it is consumed by the next absorb).
    size_t n = m_trees->pending.size();
    for (size_t i = 0; i < n; ++i) {
        if (m_trees->pending.get(i) == k) {
            m_cache->pending_seen.insert(k);
            return;
        }
    }
    m_trees->pending.add(k);
    m_cache->pending_seen.insert(k);
    m_cache->synced_version = uint64_t(-1); // re-sync on the next search
}

// Absorb (write transaction) or compute the read-only overlay: the set of keys the
// persisted graph does not reflect yet, answered by brute force at query time.
void VectorIndex::ensure_synced(const Table& table)
{
    uint64_t cur = table.get_content_version();
    if (m_cache->synced_version == cur)
        return;

    m_cache->overlay.clear();
    m_cache->stale.clear();

    if (!get_alloc().is_read_only()) {
        absorb(table);
        m_cache->synced_version = cur;
        return;
    }

    ensure_key_map();
    size_t d = dim();
    size_t n_pending = m_trees->pending.size();
    for (size_t i = 0; i < n_pending; ++i) {
        int64_t key = m_trees->pending.get(i);
        if (m_cache->key2id.count(key))
            m_cache->stale.insert(key); // graph copy outdated; brute-force from table data
    }
    for (auto obj : table) {
        auto lst = obj.get_list<float>(m_column);
        int64_t key = obj.get_key().value;
        if (d != 0 && lst.size() == d && !m_cache->key2id.count(key))
            m_cache->overlay.push_back(key); // not absorbed yet
        else if (d == 0 && lst.size() > 0)
            m_cache->overlay.push_back(key); // graph never built with data
        else if (m_cache->stale.count(key))
            m_cache->overlay.push_back(key); // pending edit
    }
    m_cache->synced_version = cur;
}

std::vector<ObjKey> VectorIndex::search(const Table& table, const std::vector<float>& query, size_t k,
                                        const std::unordered_set<uint64_t>* candidates, size_t ef_override)
{
    std::vector<ObjKey> out;
    if (query.empty() || k == 0 || (candidates && candidates->empty()))
        return out;

    std::lock_guard<std::mutex> lock(m_cache->mutex);
    ensure_synced(table);

    Trees& t = *m_trees;
    size_t d = dim();
    if (d != 0 && query.size() != d)
        throw IllegalOperation("Query vector dimension does not match the vector index");

    const float* q = query.data();
    std::vector<float> qnorm;
    if (m_config.metric == VectorMetric::Cosine) {
        qnorm = query;
        normalize_vec(qnorm.data(), qnorm.size());
        q = qnorm.data();
    }

    std::vector<GraphOps::Hit> hits; // (distance, ObjKey value)

    int64_t entry = t.hdr(h_entry) - 1;
    if (d != 0 && entry >= 0) {
        GraphOps ops{t, m_config, m_config.metric, d};
        std::vector<float> scratch(d);

        int64_t curr = entry;
        for (int lc = ops.max_level(); lc > 0; --lc)
            curr = ops.greedy(q, curr, lc, scratch);

        // Standard HNSW semantics: the beam is ef_search (bounded below by k),
        // overridable per query. Results are approximate; raise the beam for
        // better recall. Heavily filtered searches self-correct: while fewer than
        // ef admissible nodes have been found the beam keeps expanding, so tiny
        // candidate sets are explored near-exhaustively.
        size_t ef = std::max(k, ef_override ? ef_override : m_config.ef_search);

        auto admit = [&](int64_t id) {
            int64_t key = t.keys.get(size_t(id));
            if (key < 0)
                return false; // tombstone
            if (m_cache->stale.count(key))
                return false; // outdated copy; the overlay answers for this key
            return !candidates || candidates->count(uint64_t(key)) != 0;
        };
        // Unfiltered: over-fetch a little so hits dropped by the validity check
        // below (objects deleted but not absorbed yet) still leave k results.
        size_t fetch = candidates ? ef : std::max(ef, k + 16);
        for (auto& h : ops.search_layer(q, curr, fetch, 0, admit, scratch))
            hits.emplace_back(h.first, t.keys.get(size_t(h.second)));
    }

    // Brute-force the overlay (small: keys not yet absorbed) with live table data.
    if (!m_cache->overlay.empty()) {
        std::vector<float> buf(query.size());
        for (int64_t key : m_cache->overlay) {
            if (candidates && !candidates->count(uint64_t(key)))
                continue;
            Obj obj = table.get_object(ObjKey(key));
            auto lst = obj.get_list<float>(m_column);
            if (lst.size() != query.size())
                continue;
            for (size_t i = 0; i < buf.size(); ++i)
                buf[i] = lst.get(i);
            if (m_config.metric == VectorMetric::Cosine)
                normalize_vec(buf.data(), buf.size());
            hits.emplace_back(metric_dist(m_config.metric, q, buf.data(), buf.size()), key);
        }
    }

    std::sort(hits.begin(), hits.end());
    out.reserve(std::min(k, hits.size()));
    for (auto& h : hits) {
        if (out.size() >= k)
            break;
        // Without a candidate filter, the graph may still hold objects deleted
        // from the table but not absorbed yet — validate before returning.
        if (!candidates && !table.is_valid(ObjKey(h.second)))
            continue;
        out.push_back(ObjKey(h.second));
    }
    return out;
}
