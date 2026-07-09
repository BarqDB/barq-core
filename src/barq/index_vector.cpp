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
#include <atomic>
#include <cmath>
#include <mutex>
#include <queue>
#include <random>
#include <thread>
#include <unordered_map>

// The NEON kernels use AArch64-only intrinsics (vaddvq_f32, vfmaq_f32); 32-bit
// ARM takes the scalar path.
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && defined(__aarch64__)
#include <arm_neon.h>
#define BARQ_VECTOR_SIMD_NEON 1
#elif defined(__SSE2__) || (defined(_M_X64) && !defined(_M_ARM64EC))
#include <immintrin.h>
#define BARQ_VECTOR_SIMD_SSE 1
#endif

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

// SIMD distance kernels (float accumulation, as hnswlib). Four independent
// lanes keep the dependency chain short for the scalar fallback too.

float dot_product(const float* a, const float* b, size_t dim)
{
#if BARQ_VECTOR_SIMD_NEON
    float32x4_t s0 = vdupq_n_f32(0), s1 = vdupq_n_f32(0);
    size_t i = 0;
    for (; i + 8 <= dim; i += 8) {
        s0 = vfmaq_f32(s0, vld1q_f32(a + i), vld1q_f32(b + i));
        s1 = vfmaq_f32(s1, vld1q_f32(a + i + 4), vld1q_f32(b + i + 4));
    }
    if (i + 4 <= dim) {
        s0 = vfmaq_f32(s0, vld1q_f32(a + i), vld1q_f32(b + i));
        i += 4;
    }
    float sum = vaddvq_f32(vaddq_f32(s0, s1));
    for (; i < dim; ++i)
        sum += a[i] * b[i];
    return sum;
#elif BARQ_VECTOR_SIMD_SSE
    __m128 s0 = _mm_setzero_ps(), s1 = _mm_setzero_ps();
    size_t i = 0;
    for (; i + 8 <= dim; i += 8) {
        s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
        s1 = _mm_add_ps(s1, _mm_mul_ps(_mm_loadu_ps(a + i + 4), _mm_loadu_ps(b + i + 4)));
    }
    if (i + 4 <= dim) {
        s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
        i += 4;
    }
    s0 = _mm_add_ps(s0, s1);
    alignas(16) float lanes[4];
    _mm_store_ps(lanes, s0);
    float sum = lanes[0] + lanes[1] + lanes[2] + lanes[3];
    for (; i < dim; ++i)
        sum += a[i] * b[i];
    return sum;
#else
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    size_t i = 0;
    for (; i + 4 <= dim; i += 4) {
        s0 += a[i] * b[i];
        s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2];
        s3 += a[i + 3] * b[i + 3];
    }
    float sum = (s0 + s1) + (s2 + s3);
    for (; i < dim; ++i)
        sum += a[i] * b[i];
    return sum;
#endif
}

float l2_sq(const float* a, const float* b, size_t dim)
{
#if BARQ_VECTOR_SIMD_NEON
    float32x4_t s0 = vdupq_n_f32(0), s1 = vdupq_n_f32(0);
    size_t i = 0;
    for (; i + 8 <= dim; i += 8) {
        float32x4_t d0 = vsubq_f32(vld1q_f32(a + i), vld1q_f32(b + i));
        float32x4_t d1 = vsubq_f32(vld1q_f32(a + i + 4), vld1q_f32(b + i + 4));
        s0 = vfmaq_f32(s0, d0, d0);
        s1 = vfmaq_f32(s1, d1, d1);
    }
    if (i + 4 <= dim) {
        float32x4_t d0 = vsubq_f32(vld1q_f32(a + i), vld1q_f32(b + i));
        s0 = vfmaq_f32(s0, d0, d0);
        i += 4;
    }
    float sum = vaddvq_f32(vaddq_f32(s0, s1));
    for (; i < dim; ++i) {
        float d = a[i] - b[i];
        sum += d * d;
    }
    return sum;
#elif BARQ_VECTOR_SIMD_SSE
    __m128 s0 = _mm_setzero_ps(), s1 = _mm_setzero_ps();
    size_t i = 0;
    for (; i + 8 <= dim; i += 8) {
        __m128 d0 = _mm_sub_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i));
        __m128 d1 = _mm_sub_ps(_mm_loadu_ps(a + i + 4), _mm_loadu_ps(b + i + 4));
        s0 = _mm_add_ps(s0, _mm_mul_ps(d0, d0));
        s1 = _mm_add_ps(s1, _mm_mul_ps(d1, d1));
    }
    if (i + 4 <= dim) {
        __m128 d0 = _mm_sub_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i));
        s0 = _mm_add_ps(s0, _mm_mul_ps(d0, d0));
        i += 4;
    }
    s0 = _mm_add_ps(s0, s1);
    alignas(16) float lanes[4];
    _mm_store_ps(lanes, s0);
    float sum = lanes[0] + lanes[1] + lanes[2] + lanes[3];
    for (; i < dim; ++i) {
        float d = a[i] - b[i];
        sum += d * d;
    }
    return sum;
#else
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    size_t i = 0;
    for (; i + 4 <= dim; i += 4) {
        float d0 = a[i] - b[i], d1 = a[i + 1] - b[i + 1];
        float d2 = a[i + 2] - b[i + 2], d3 = a[i + 3] - b[i + 3];
        s0 += d0 * d0;
        s1 += d1 * d1;
        s2 += d2 * d2;
        s3 += d3 * d3;
    }
    float sum = (s0 + s1) + (s2 + s3);
    for (; i < dim; ++i) {
        float d = a[i] - b[i];
        sum += d * d;
    }
    return sum;
#endif
}

// Distance where smaller = closer (hnswlib convention).
float metric_dist(VectorMetric metric, const float* a, const float* b, size_t dim)
{
    if (metric == VectorMetric::L2)
        return l2_sq(a, b, dim);
    // Inner product / cosine (vectors pre-normalized for cosine).
    return 1.0f - dot_product(a, b, dim);
}

// Level assignment: deterministic per id (exponential distribution, as hnswlib).
// Shared by the incremental and bulk build paths so the same id always lands on
// the same layer no matter which path inserted it.
int hnsw_level_for(int64_t id, uint64_t salt, size_t m)
{
    uint64_t h = splitmix64(uint64_t(id) ^ salt);
    double u = (double(h >> 11) + 1.0) / 9007199254740993.0; // (0, 1)
    double mult = 1.0 / std::log(double(m));
    return int(-std::log(u) * mult);
}

// Effective search beam for an auto (ef_search == 0) config: wider beams for
// bigger graphs hold recall roughly flat as the table grows (measured ~98%
// recall@10 on Deep1B at 100k with 64 and at 1M with 128).
size_t auto_ef_search(size_t live_count)
{
    if (live_count <= 100'000)
        return 64;
    if (live_count <= 1'000'000)
        return 128;
    if (live_count <= 10'000'000)
        return 192;
    return 256;
}

// Reusable visited-set for graph traversal: epoch-tagged so clearing between
// searches is one counter bump instead of an O(n) wipe or a hash set rebuild.
struct VisitedTags {
    std::vector<uint32_t> tags;
    uint32_t epoch = 0;

    void begin(size_t n)
    {
        if (tags.size() < n)
            tags.resize(n, 0);
        if (++epoch == 0) {
            std::fill(tags.begin(), tags.end(), 0);
            epoch = 1;
        }
    }
    // Returns true if already visited; marks visited otherwise.
    bool test_and_set(int64_t id)
    {
        uint32_t& tag = tags[size_t(id)];
        if (tag == epoch)
            return true;
        tag = epoch;
        return false;
    }
};

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

    // Scratch for graph traversal (searches serialize on the mutex below, so one
    // instance per accessor suffices). Survives across queries: clearing is an
    // epoch bump, not a wipe.
    VisitedTags visited;

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
        t.vectors.get_range(size_t(id) * dim, dim, out);
    }

    float dist_to(const float* q, int64_t id, std::vector<float>& scratch) const
    {
        read_vec(id, scratch.data());
        return metric_dist(metric, q, scratch.data(), dim);
    }

    void neighbors(int64_t id, int layer, std::vector<int64_t>& out) const
    {
        const BPlusTree<int64_t>* tree;
        size_t base;
        if (layer == 0) {
            tree = &t.links0;
            base = size_t(id) * stride0();
        }
        else {
            tree = &t.links_upper;
            int64_t block = t.upper_ofs.get(size_t(id));
            base = size_t(block + layer - 1) * stride_up();
        }
        size_t cnt = size_t(tree->get(base));
        out.resize(cnt);
        if (cnt)
            tree->get_range(base + 1, cnt, out.data());
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
                                  std::vector<float>& scratch, VisitedTags& visited) const
    {
        std::priority_queue<Hit> results;                                 // max-heap: worst admitted on top
        std::priority_queue<Hit, std::vector<Hit>, std::greater<>> cands; // min-heap: closest first
        visited.begin(size_t(total()));

        float d0 = dist_to(q, entry_point, scratch);
        cands.emplace(d0, entry_point);
        visited.test_and_set(entry_point);
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
                if (visited.test_and_set(n))
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

    int level_for(int64_t id) const
    {
        return hnsw_level_for(id, uint64_t(t.hdr(h_salt)), cfg.m);
    }
};

// ---- bulk (in-RAM) graph builder -------------------------------------------------
//
// Rebuilds and first-time builds run here: the graph is assembled in flat memory
// (same algorithm as insert_element, ported like it from hnswlib) and the finished
// arrays are appended to the persisted B+-trees in one sequential pass. Graph
// traversal during construction then costs plain loads and stores instead of
// B+-tree descents and copy-on-write leaf clones, and inserts run on every core:
// a striped lock per node guards its adjacency lists, the entry point swaps under
// a global lock, and every list access is lock-copy-unlock, so no two locks are
// ever held at once (hnswlib's locking scheme).
class BulkBuilder {
public:
    BulkBuilder(const VectorIndexConfig& cfg, size_t dim, uint64_t salt)
        : m_cfg(cfg)
        , m_dim(dim)
        , m_salt(salt)
    {
    }

    size_t size() const
    {
        return m_keys.size();
    }
    int64_t key(size_t id) const
    {
        return m_keys[id];
    }
    const float* vec(size_t id) const
    {
        return m_vecs.data() + id * m_dim;
    }

    void reserve(size_t n)
    {
        m_keys.reserve(n);
        m_vecs.reserve(n * m_dim);
        m_levels.reserve(n);
        m_upper_ofs.reserve(n);
    }

    // Collect phase. `v` must already be normalized for the cosine metric.
    void add(int64_t key, const float* v)
    {
        int64_t id = int64_t(m_keys.size());
        m_keys.push_back(key);
        m_vecs.insert(m_vecs.end(), v, v + m_dim);
        int level = hnsw_level_for(id, m_salt, m_cfg.m);
        m_levels.push_back(level);
        if (level > 0) {
            m_upper_ofs.push_back(int64_t(m_upper_blocks));
            m_upper_blocks += size_t(level);
        }
        else {
            m_upper_ofs.push_back(-1);
        }
    }

    // Insert every collected element into the graph, in parallel.
    void build(size_t n_threads)
    {
        size_t n = m_keys.size();
        m_links0.assign(n * stride0(), 0);
        m_links_upper.assign(m_upper_blocks * stride_up(), 0);
        if (n == 0)
            return;
        // Allocated here, not in the constructor: absorb creates a builder as its
        // delta buffer on every sync, and most syncs never build anything.
        m_locks = std::make_unique<std::mutex[]>(kStripes);

        Scratch scr;
        insert_one(0, scr); // seed the entry point serially
        if (n == 1)
            return;

        n_threads = std::max<size_t>(1, std::min(n_threads, 1 + (n - 1) / 256));
        if (n_threads == 1) {
            for (size_t id = 1; id < n; ++id)
                insert_one(int64_t(id), scr);
        }
        else {
            std::atomic<size_t> next{1};
            std::exception_ptr first_error;
            std::mutex error_mutex;
            auto worker = [&] {
                Scratch wscr;
                try {
                    for (size_t id = next.fetch_add(1); id < n; id = next.fetch_add(1))
                        insert_one(int64_t(id), wscr);
                }
                catch (...) {
                    std::lock_guard<std::mutex> lock(error_mutex);
                    if (!first_error)
                        first_error = std::current_exception();
                    next.store(n); // stop the other workers
                }
            };
            std::vector<std::thread> pool;
            pool.reserve(n_threads);
            for (size_t i = 0; i < n_threads; ++i)
                pool.emplace_back(worker);
            for (auto& th : pool)
                th.join();
            if (first_error)
                std::rethrow_exception(first_error);
        }
        repair_connectivity(scr);
    }

    // Append the finished graph to the (empty) persisted trees and set the
    // graph-shape header fields. One sequential pass per array.
    void write(VectorIndex::Trees& t) const
    {
        size_t n = m_keys.size();
        for (size_t id = 0; id < n; ++id)
            t.keys.add(m_keys[id]);
        for (size_t id = 0; id < n; ++id)
            t.levels.add(m_levels[id]);
        for (int32_t v : m_links0)
            t.links0.add(v);
        for (size_t id = 0; id < n; ++id)
            t.upper_ofs.add(m_upper_ofs[id]);
        for (int32_t v : m_links_upper)
            t.links_upper.add(v);
        for (float x : m_vecs)
            t.vectors.add(x);
        t.set_hdr(h_entry, m_entry + 1);
        t.set_hdr(h_maxlevel, (m_entry < 0 ? 0 : m_entry_level + 1));
        t.set_hdr(h_total, int64_t(n));
        t.set_hdr(h_deleted, 0);
    }

private:
    using Hit = std::pair<float, int64_t>; // (distance, id)

    struct Scratch {
        VisitedTags visited;
        std::vector<int64_t> nbrs;
        std::vector<int64_t> selected;
    };

    size_t stride0() const
    {
        return 2 * m_cfg.m + 1;
    }
    size_t stride_up() const
    {
        return m_cfg.m + 1;
    }

    float dist(const float* a, const float* b) const
    {
        return metric_dist(m_cfg.metric, a, b, m_dim);
    }

    std::mutex& stripe(int64_t id)
    {
        return m_locks[size_t(id) & (kStripes - 1)];
    }

    // The adjacency block of (id, layer). Reads and writes take the node's stripe.
    int32_t* links(int64_t id, int layer)
    {
        if (layer == 0)
            return m_links0.data() + size_t(id) * stride0();
        return m_links_upper.data() + size_t(m_upper_ofs[size_t(id)] + layer - 1) * stride_up();
    }

    void copy_links(int64_t id, int layer, std::vector<int64_t>& out)
    {
        std::lock_guard<std::mutex> lock(stripe(id));
        const int32_t* p = links(id, layer);
        out.assign(p + 1, p + 1 + p[0]);
    }

    void store_links(int32_t* p, const std::vector<int64_t>& list)
    {
        p[0] = int32_t(list.size());
        for (size_t i = 0; i < list.size(); ++i)
            p[1 + i] = int32_t(list[i]);
    }

    int64_t greedy(const float* q, int64_t curr, int layer, Scratch& scr)
    {
        float best = dist(q, vec(size_t(curr)));
        bool changed = true;
        while (changed) {
            changed = false;
            copy_links(curr, layer, scr.nbrs);
            for (int64_t n : scr.nbrs) {
                float d = dist(q, vec(size_t(n)));
                if (d < best) {
                    best = d;
                    curr = n;
                    changed = true;
                }
            }
        }
        return curr;
    }

    std::vector<Hit> search_layer(const float* q, int64_t entry_point, size_t ef, int layer, Scratch& scr)
    {
        std::priority_queue<Hit> results;
        std::priority_queue<Hit, std::vector<Hit>, std::greater<>> cands;
        scr.visited.begin(m_keys.size());

        float d0 = dist(q, vec(size_t(entry_point)));
        cands.emplace(d0, entry_point);
        scr.visited.test_and_set(entry_point);
        results.emplace(d0, entry_point);

        while (!cands.empty()) {
            Hit c = cands.top();
            if (results.size() >= ef && c.first > results.top().first)
                break;
            cands.pop();

            copy_links(c.second, layer, scr.nbrs);
            for (int64_t n : scr.nbrs) {
                if (scr.visited.test_and_set(n))
                    continue;
                float d = dist(q, vec(size_t(n)));
                if (results.size() < ef || d < results.top().first) {
                    cands.emplace(d, n);
                    results.emplace(d, n);
                    if (results.size() > ef)
                        results.pop();
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

    // Same spread-out heuristic as GraphOps::select_neighbors, on flat memory.
    void select_neighbors(const std::vector<Hit>& cands_sorted, size_t m, std::vector<int64_t>& result) const
    {
        result.clear();
        if (cands_sorted.size() <= m) {
            for (auto& c : cands_sorted)
                result.push_back(c.second);
            return;
        }
        for (auto& c : cands_sorted) {
            if (result.size() >= m)
                break;
            const float* cv = vec(size_t(c.second));
            bool good = true;
            for (int64_t r : result) {
                if (dist(cv, vec(size_t(r))) < c.first) {
                    good = false;
                    break;
                }
            }
            if (good)
                result.push_back(c.second);
        }
    }

    // Concurrent inserts can strand nodes: two nearby elements inserted at the
    // same time cannot see each other, so both lean on the same busy neighbours,
    // whose overflow re-selection may then drop every path back to one of them.
    // A node (or island of nodes) that layer-0 traversal cannot reach from the
    // entry point is invisible to queries. Sweep: BFS the out-edges from the
    // entry, give every unreached node an in-link from a reached near neighbour
    // (a free slot when available, else evicting a link whose target keeps other
    // in-links), and let the node's own out-edges pull in the rest of its
    // island. Re-verify in case an eviction disconnected something.
    void repair_connectivity(Scratch& scr)
    {
        size_t n = m_keys.size();
        if (n < 2 || m_entry < 0)
            return;
        size_t s0 = stride0();

        std::vector<int32_t> indeg(n, 0);
        for (size_t id = 0; id < n; ++id) {
            const int32_t* p = m_links0.data() + id * s0;
            for (int32_t i = 0; i < p[0]; ++i)
                indeg[size_t(p[1 + i])]++;
        }

        std::vector<uint8_t> reached(n, 0);
        std::vector<int64_t> queue;
        auto bfs_from = [&](int64_t root) {
            if (reached[size_t(root)])
                return;
            reached[size_t(root)] = 1;
            queue.assign(1, root);
            while (!queue.empty()) {
                int64_t v = queue.back();
                queue.pop_back();
                const int32_t* p = m_links0.data() + size_t(v) * s0;
                for (int32_t i = 0; i < p[0]; ++i) {
                    int32_t w = p[1 + i];
                    if (!reached[size_t(w)]) {
                        reached[size_t(w)] = 1;
                        queue.push_back(w);
                    }
                }
            }
        };

        // Anchor sweeps, each verified by the fresh BFS at the top of the next
        // iteration (an eviction may itself disconnect a node, so a sweep's own
        // bookkeeping is not trusted). The last sweep appends into free slots
        // only — it cannot evict, so it cannot strand anything — which makes the
        // closing BFS authoritative.
        bool complete = false;
        constexpr int rounds = 6;
        for (int round = 0; round <= rounds && !complete; ++round) {
            std::fill(reached.begin(), reached.end(), 0);
            bfs_from(m_entry);
            complete = true;
            for (size_t id = 0; id < n; ++id) {
                if (reached[id])
                    continue;
                complete = false;
                if (round == rounds)
                    break; // out of repair rounds; keep the BFS verdict
                if (anchor(int64_t(id), indeg, scr, /*allow_evict=*/round + 1 < rounds))
                    bfs_from(int64_t(id)); // absorbs the node's island downstream
            }
        }
        // Never observed to fail; if it ever does, the graph degrades to plain
        // (unrepaired) hnswlib-grade approximation instead of misbehaving.
        BARQ_ASSERT_DEBUG(complete);
        static_cast<void>(complete);
    }

    // Give `x` an in-link from a node reachable from the entry point. The anchor
    // candidates come from a beam search rooted at the entry, so every candidate
    // is itself reachable.
    bool anchor(int64_t x, std::vector<int32_t>& indeg, Scratch& scr, bool allow_evict)
    {
        auto anchors = search_layer(vec(size_t(x)), m_entry, m_cfg.ef_construction, 0, scr);
        size_t max_m0 = 2 * m_cfg.m;
        for (auto& a : anchors) { // nearest first
            if (a.second == x)
                continue;
            int32_t* p = links(a.second, 0);
            if (size_t(p[0]) < max_m0) {
                p[1 + p[0]] = int32_t(x);
                p[0]++;
                indeg[size_t(x)]++;
                return true;
            }
        }
        if (!allow_evict)
            return false;
        // Every anchor is full: evict the farthest link of the nearest anchor,
        // preferring one whose target keeps other in-links.
        for (auto& a : anchors) {
            if (a.second == x)
                continue;
            int32_t* p = links(a.second, 0);
            const float* av = vec(size_t(a.second));
            int best = -1, fallback = -1;
            float best_d = -1, fallback_d = -1;
            for (int32_t i = 0; i < p[0]; ++i) {
                float dd = dist(av, vec(size_t(p[1 + i])));
                if (dd > fallback_d) {
                    fallback_d = dd;
                    fallback = i;
                }
                if (indeg[size_t(p[1 + i])] >= 2 && dd > best_d) {
                    best_d = dd;
                    best = i;
                }
            }
            if (best < 0)
                best = fallback;
            if (best < 0)
                continue; // empty list; try the next anchor
            int64_t evicted = p[1 + best];
            p[1 + best] = int32_t(x);
            indeg[size_t(x)]++;
            indeg[size_t(evicted)]--;
            return true;
        }
        return false;
    }

    // Port of hnswlib::addPoint's concurrent insert (minus deletes, which cannot
    // occur during a bulk build). hnswlib holds the new element's link lock for
    // the whole insert so nobody can interact with a half-inserted node; with
    // striped locks that would deadlock, so the same protection comes from two
    // passes: first search every layer and store the node's own adjacency lists
    // while the node has no in-links (invisible to concurrent searches), then
    // publish it by appending the backlinks. Once discoverable, its own lists
    // are final and never overwritten, so no concurrent append can be lost.
    void insert_one(int64_t id, Scratch& scr)
    {
        const float* q = vec(size_t(id));
        int level = m_levels[size_t(id)];

        std::unique_lock<std::mutex> entry_lock(m_entry_mutex);
        int64_t curr = m_entry;
        int max_level = m_entry_level;
        if (curr < 0) {
            m_entry = id;
            m_entry_level = level;
            return;
        }
        // Only inserts that will raise the max level keep the global lock; every
        // other insert proceeds concurrently.
        if (level <= max_level)
            entry_lock.unlock();

        for (int lc = max_level; lc > level; --lc)
            curr = greedy(q, curr, lc, scr);

        // Pass 1: pick and store this node's own neighbours on every layer.
        int top = std::min(level, max_level);
        std::vector<std::vector<int64_t>> selected_per_layer(size_t(top) + 1);
        for (int lc = top; lc >= 0; --lc) {
            auto cands = search_layer(q, curr, m_cfg.ef_construction, lc, scr);
            select_neighbors(cands, m_cfg.m, scr.selected);
            {
                std::lock_guard<std::mutex> lock(stripe(id));
                store_links(links(id, lc), scr.selected);
            }
            selected_per_layer[size_t(lc)] = scr.selected;
            if (!scr.selected.empty())
                curr = scr.selected.front();
        }

        // Pass 2: publish — connect the selected neighbours back to the node.
        std::vector<int64_t> keep;
        std::vector<Hit> merged;
        for (int lc = top; lc >= 0; --lc) {
            size_t max_m = (lc == 0) ? 2 * m_cfg.m : m_cfg.m;
            for (int64_t n : selected_per_layer[size_t(lc)]) {
                std::lock_guard<std::mutex> lock(stripe(n));
                int32_t* p = links(n, lc);
                if (size_t(p[0]) < max_m) {
                    p[1 + p[0]] = int32_t(id);
                    p[0]++;
                }
                else {
                    // Overflow: re-select the neighbour's links with the heuristic,
                    // considering the new element too (as hnswlib does).
                    const float* nv = vec(size_t(n));
                    merged.clear();
                    merged.emplace_back(dist(nv, q), id);
                    for (int32_t i = 0; i < p[0]; ++i)
                        merged.emplace_back(dist(nv, vec(size_t(p[1 + i]))), int64_t(p[1 + i]));
                    std::sort(merged.begin(), merged.end());
                    select_neighbors(merged, max_m, keep);
                    store_links(p, keep);
                }
            }
        }

        if (level > max_level) { // entry_lock still held
            m_entry = id;
            m_entry_level = level;
        }
    }

    static constexpr size_t kStripes = 8192;

    VectorIndexConfig m_cfg;
    size_t m_dim;
    uint64_t m_salt;

    std::vector<int64_t> m_keys;
    std::vector<float> m_vecs;
    std::vector<int32_t> m_levels;
    std::vector<int64_t> m_upper_ofs;
    size_t m_upper_blocks = 0;
    std::vector<int32_t> m_links0;
    std::vector<int32_t> m_links_upper;

    std::mutex m_entry_mutex;
    int64_t m_entry = -1;
    int m_entry_level = -1;
    std::unique_ptr<std::mutex[]> m_locks; // allocated by build()
};

// Build-time worker count: `configured` wins when set; otherwise one per core.
size_t effective_build_threads(size_t configured)
{
    if (configured)
        return configured;
    size_t hw = std::thread::hardware_concurrency();
    return hw ? hw : 1;
}

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
void VectorIndex::insert_element(int64_t key, const float* vec, size_t d)
{
    Trees& t = *m_trees;
    GraphOps ops{t, m_config, m_config.metric, d};

    int64_t id = t.hdr(h_total);
    int level = ops.level_for(id);

    t.keys.add(key);
    t.levels.add(level);
    for (size_t i = 0; i < d; ++i)
        t.vectors.add(vec[i]);
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

    std::vector<float> scratch_a(d), scratch_b(d);
    const float* q = vec;
    int64_t curr = entry;
    for (int lc = max_level; lc > level; --lc)
        curr = ops.greedy(q, curr, lc, scratch_a);

    auto admit_all = [](int64_t) {
        return true;
    };
    for (int lc = std::min(level, max_level); lc >= 0; --lc) {
        auto cands = ops.search_layer(q, curr, m_config.ef_construction, lc, admit_all, scratch_a, m_cache->visited);
        auto selected = ops.select_neighbors(cands, m_config.m, scratch_a, scratch_b);
        ops.set_neighbors(id, lc, selected);

        size_t max_m = (lc == 0) ? 2 * m_config.m : m_config.m;
        std::vector<int64_t> nbrs;
        std::vector<float> nvec(d);
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
                merged.emplace_back(metric_dist(m_config.metric, nvec.data(), scratch_a.data(), d), id);
                for (int64_t x : nbrs) {
                    ops.read_vec(x, scratch_a.data());
                    merged.emplace_back(metric_dist(m_config.metric, nvec.data(), scratch_a.data(), d), x);
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

namespace {

// Empty the graph arrays and reset the graph-shape header fields; dim and salt
// are left alone. The persisted trees are then ready for a BulkBuilder::write.
void clear_graph_arrays(VectorIndex::Trees& t)
{
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
}

// Build the graph in RAM from the builder's collected elements, persist it, and
// refresh the key map. The previous graph contents (if any) are discarded — but
// only after the long, fallible build phase has succeeded.
void bulk_rebuild(VectorIndex::Trees& t, VectorIndex::Cache& cache, size_t build_threads, BulkBuilder& builder)
{
    builder.build(effective_build_threads(build_threads));
    clear_graph_arrays(t);
    cache.pending_seen.clear(); // mirrors the persisted pending list, emptied above
    // Invalidate the key map across the append: if write() throws and the caller
    // catches inside the transaction, the next absorb must rebuild the map from
    // the actual (cleared) trees instead of tombstoning through a stale one.
    cache.key2id.clear();
    cache.key2id_valid = false;
    builder.write(t);
    for (size_t id = 0; id < builder.size(); ++id)
        cache.key2id[builder.key(id)] = int64_t(id);
    cache.key2id_valid = true;
}

} // anonymous namespace

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
            m_cache->pending_seen.clear();
            return;
        }
        t.set_hdr(h_dim, int64_t(d));
    }

    // Pending in-place edits: tombstone the stale copy; the delta pass below
    // re-inserts the edited keys as if they were new. The consumed keys leave
    // pending_seen too, or a second edit of the same object later in this
    // transaction would be mistaken for already-recorded and never re-indexed.
    size_t n_pending = t.pending.size();
    for (size_t i = 0; i < n_pending; ++i) {
        int64_t key = t.pending.get(i);
        auto it = m_cache->key2id.find(key);
        if (it != m_cache->key2id.end()) {
            tombstone(it->second);
            m_cache->key2id.erase(it);
        }
    }
    if (n_pending) {
        t.pending.clear();
        m_cache->pending_seen.clear();
    }

    // One pass over the table: collect the unindexed keys (with their vectors)
    // and the set of live keys.
    BulkBuilder delta(m_config, d, uint64_t(t.hdr(h_salt)));
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
        lst.get_tree().get_range(0, d, buf.data());
        if (m_config.metric == VectorMetric::Cosine)
            normalize_vec(buf.data(), d);
        delta.add(key, buf.data());
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

    int64_t live = t.hdr(h_total) - t.hdr(h_deleted);
    if (delta.size() > 0 && live == 0) {
        // Nothing live in the graph: build it fresh in RAM from the collected
        // delta — orders of magnitude faster than one-at-a-time inserts.
        bulk_rebuild(t, *m_cache, m_config.build_threads, delta);
        return;
    }
    if (int64_t(delta.size()) > live) {
        // The new data dominates the existing graph: rebuilding everything in
        // RAM beats growing the persisted graph incrementally.
        do_rebuild(table);
        return;
    }

    for (size_t i = 0; i < delta.size(); ++i)
        insert_element(delta.key(i), delta.vec(i), d);

    // Compact once tombstones dominate: rebuild from live data.
    int64_t deleted = t.hdr(h_deleted);
    if (deleted > 0 && deleted * 2 >= t.hdr(h_total))
        do_rebuild(table);
}

void VectorIndex::do_rebuild(const Table& table)
{
    Trees& t = *m_trees;
    clear_graph_arrays(t);
    t.set_hdr(h_dim, 0);
    m_cache->pending_seen.clear(); // mirrors the persisted pending list, emptied above
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

    BulkBuilder builder(m_config, d, uint64_t(t.hdr(h_salt)));
    builder.reserve(table.size());
    std::vector<float> buf(d);
    for (auto obj : table) {
        auto lst = obj.get_list<float>(m_column);
        if (lst.size() != d)
            continue;
        lst.get_tree().get_range(0, d, buf.data());
        if (m_config.metric == VectorMetric::Cosine)
            normalize_vec(buf.data(), d);
        builder.add(obj.get_key().value, buf.data());
    }
    bulk_rebuild(t, *m_cache, m_config.build_threads, builder);
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
        // better recall. An auto config (ef_search == 0, the default) widens the
        // beam as the graph grows, holding recall roughly flat with scale.
        // Heavily filtered searches self-correct: while fewer than ef admissible
        // nodes have been found the beam keeps expanding, so tiny candidate sets
        // are explored near-exhaustively.
        size_t base_ef = ef_override ? ef_override : m_config.ef_search;
        if (base_ef == 0)
            base_ef = auto_ef_search(count());
        size_t ef = std::max(k, base_ef);

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
        for (auto& h : ops.search_layer(q, curr, fetch, 0, admit, scratch, m_cache->visited))
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
            lst.get_tree().get_range(0, buf.size(), buf.data());
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
