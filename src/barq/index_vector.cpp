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

#include <barq/list.hpp>
#include <barq/table.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <sstream>

// hnswlib is vendored third-party (Apache 2.0); silence its internal warnings.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wreorder-ctor"
#pragma clang diagnostic ignored "-Wconditional-uninitialized"
#pragma clang diagnostic ignored "-Wunused-parameter"
#endif
#include <external/hnswlib/hnswlib.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

using namespace barq;

namespace {

// Blob layout: a fixed header of uint64 fields, then the serialized hnswlib graph.
constexpr size_t s_hdr_magic = 0;
constexpr size_t s_hdr_format = 1;
constexpr size_t s_hdr_dim = 2;
constexpr size_t s_hdr_count = 3;
constexpr size_t s_hdr_rows = 4;
constexpr size_t s_hdr_metric = 5;
constexpr size_t s_hdr_m = 6;
constexpr size_t s_hdr_efc = 7;
constexpr size_t s_hdr_efs = 8;
constexpr size_t s_hdr_fields = 9;
constexpr size_t s_hdr_bytes = s_hdr_fields * sizeof(uint64_t);
constexpr uint64_t s_magic = 0x42'41'52'51'56'45'43'31ULL; // "BARQVEC1"
constexpr uint64_t s_format = 2;

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

// Serialize a graph is capped to a single ArrayBlob node; larger indexes would need a
// chunked/big-blob store (a later refinement). Guard against silently truncating.
constexpr size_t s_max_blob = ArrayBlob::max_binary_size;

// The index-cache graph operations are serialized; the in-memory graph is a lazily
// (re)built cache shared by whichever thread queries the column.
std::mutex g_vector_index_mutex;

} // anonymous namespace

// The in-memory graph. m_space must outlive m_hnsw (the graph captures the space's
// distance-func param), so declaration order matters.
struct VectorIndex::Graph {
    std::unique_ptr<hnswlib::SpaceInterface<float>> space;
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> hnsw;
};

namespace {

std::unique_ptr<hnswlib::SpaceInterface<float>> make_space(VectorMetric metric, size_t dim)
{
    if (metric == VectorMetric::L2)
        return std::make_unique<hnswlib::L2Space>(dim);
    // Cosine uses the inner-product space over normalized vectors.
    return std::make_unique<hnswlib::InnerProductSpace>(dim);
}

} // anonymous namespace

VectorIndex::VectorIndex(ColKey column, Allocator& alloc, const VectorIndexConfig& config)
    : m_column(column)
    , m_blob(alloc)
    , m_config(config)
{
    m_blob.create(); // empty blob (no header yet -> count()==0 means "not built")
}

VectorIndex::VectorIndex(ref_type ref, ArrayParent* parent, size_t ndx_in_parent, ColKey column, Allocator& alloc)
    : m_column(column)
    , m_blob(alloc)
{
    m_blob.init_from_ref(ref);
    m_blob.set_parent(parent, ndx_in_parent);
    load_config_from_header();
}

VectorIndex::~VectorIndex() = default;

Allocator& VectorIndex::get_alloc() const noexcept
{
    return m_blob.get_alloc();
}

void VectorIndex::destroy() noexcept
{
    m_blob.destroy_deep();
}

bool VectorIndex::is_attached() const noexcept
{
    return m_blob.is_attached();
}

void VectorIndex::set_parent(ArrayParent* parent, size_t ndx_in_parent) noexcept
{
    m_blob.set_parent(parent, ndx_in_parent);
}

size_t VectorIndex::get_ndx_in_parent() const noexcept
{
    return m_blob.get_ndx_in_parent();
}

void VectorIndex::update_from_parent() noexcept
{
    std::lock_guard<std::mutex> lock(g_vector_index_mutex);
    m_blob.init_from_parent();
    m_graph.reset();
    m_graph_version = uint64_t(-1);
    m_tried_load = false;
    load_config_from_header();
}

void VectorIndex::refresh_accessor_tree()
{
    std::lock_guard<std::mutex> lock(g_vector_index_mutex);
    m_blob.init_from_parent();
    m_graph.reset();
    m_graph_version = uint64_t(-1);
    m_tried_load = false;
    load_config_from_header();
}

ref_type VectorIndex::get_ref() const noexcept
{
    return m_blob.get_ref();
}

uint64_t VectorIndex::header_field(size_t index) const
{
    size_t sz = m_blob.is_attached() ? m_blob.size() : 0;
    if (sz < s_hdr_bytes)
        return 0;
    uint64_t v = 0;
    std::memcpy(&v, m_blob.get(0) + index * sizeof(uint64_t), sizeof(uint64_t));
    return v;
}

size_t VectorIndex::dim() const
{
    return size_t(header_field(s_hdr_dim));
}

size_t VectorIndex::count() const
{
    return size_t(header_field(s_hdr_count));
}

size_t VectorIndex::stored_row_count() const
{
    return size_t(header_field(s_hdr_rows));
}

void VectorIndex::load_config_from_header()
{
    if (header_field(s_hdr_magic) != s_magic || header_field(s_hdr_format) != s_format)
        return; // never built (or unknown format): keep the current config
    m_config.metric = VectorMetric(uint8_t(header_field(s_hdr_metric)));
    m_config.m = size_t(header_field(s_hdr_m));
    m_config.ef_construction = size_t(header_field(s_hdr_efc));
    m_config.ef_search = size_t(header_field(s_hdr_efs));
}

void VectorIndex::build_in_memory(const Table& table)
{
    size_t dim = 0;
    for (auto obj : table) {
        auto lst = obj.get_list<float>(m_column);
        if (lst.size() > 0) {
            dim = lst.size();
            break;
        }
    }
    m_graph_dim = dim;
    if (dim == 0) {
        m_graph.reset();
        return;
    }

    auto g = std::make_unique<Graph>();
    g->space = make_space(m_config.metric, dim);
    size_t capacity = std::max<size_t>(table.size(), 1);
    g->hnsw = std::make_unique<hnswlib::HierarchicalNSW<float>>(g->space.get(), capacity, m_config.m,
                                                                m_config.ef_construction);

    std::vector<float> buf(dim);
    for (auto obj : table) {
        auto lst = obj.get_list<float>(m_column);
        if (lst.size() != dim)
            continue;
        for (size_t i = 0; i < dim; ++i)
            buf[i] = lst.get(i);
        if (m_config.metric == VectorMetric::Cosine)
            normalize_vec(buf.data(), dim);
        g->hnsw->addPoint(buf.data(), hnswlib::labeltype(obj.get_key().value));
    }
    m_graph = std::move(g);
}

bool VectorIndex::load_from_blob()
{
    size_t sz = m_blob.is_attached() ? m_blob.size() : 0;
    if (sz < s_hdr_bytes)
        return false;
    std::string all(m_blob.get(0), sz);

    uint64_t hdr[s_hdr_fields];
    std::memcpy(hdr, all.data(), s_hdr_bytes);
    if (hdr[s_hdr_magic] != s_magic || hdr[s_hdr_format] != s_format)
        return false;
    size_t dim = size_t(hdr[s_hdr_dim]);
    size_t count = size_t(hdr[s_hdr_count]);
    if (dim == 0 || count == 0)
        return false;

    std::istringstream iss(all.substr(s_hdr_bytes), std::ios::binary);
    auto g = std::make_unique<Graph>();
    g->space = make_space(VectorMetric(uint8_t(hdr[s_hdr_metric])), dim);
    g->hnsw = std::make_unique<hnswlib::HierarchicalNSW<float>>(g->space.get());
    g->hnsw->loadIndex(iss, g->space.get());
    m_graph = std::move(g);
    m_graph_dim = dim;
    return true;
}

void VectorIndex::store_to_blob(size_t row_count)
{
    std::string graph_bytes;
    uint64_t count = 0;
    if (m_graph && m_graph->hnsw) {
        std::ostringstream oss(std::ios::binary);
        m_graph->hnsw->saveIndex(oss);
        graph_bytes = oss.str();
        count = m_graph->hnsw->cur_element_count;
    }

    uint64_t hdr[s_hdr_fields];
    hdr[s_hdr_magic] = s_magic;
    hdr[s_hdr_format] = s_format;
    hdr[s_hdr_dim] = uint64_t(m_graph_dim);
    hdr[s_hdr_count] = count;
    hdr[s_hdr_rows] = uint64_t(row_count);
    hdr[s_hdr_metric] = uint64_t(m_config.metric);
    hdr[s_hdr_m] = uint64_t(m_config.m);
    hdr[s_hdr_efc] = uint64_t(m_config.ef_construction);
    hdr[s_hdr_efs] = uint64_t(m_config.ef_search);

    std::string content;
    content.reserve(s_hdr_bytes + graph_bytes.size());
    content.append(reinterpret_cast<const char*>(hdr), s_hdr_bytes);
    content.append(graph_bytes);

    if (content.size() > s_max_blob) {
        // A single ArrayBlob node cannot hold this yet; leave the index unbuilt rather
        // than corrupt the store. (Chunked/big-blob storage is a later refinement.)
        if (m_blob.size() > 0)
            m_blob.erase(0, m_blob.size());
        return;
    }

    if (m_blob.size() > 0)
        m_blob.erase(0, m_blob.size());
    m_blob.add(content.data(), content.size());
}

void VectorIndex::ensure_graph(const Table& table)
{
    uint64_t cur = table.get_content_version();
    if (m_graph && m_graph->hnsw && m_graph_version == cur)
        return;

    if (!m_graph && !m_tried_load) {
        m_tried_load = true;
        if (count() > 0)
            load_from_blob();
    }

    if (!m_graph || !m_graph->hnsw) {
        build_in_memory(table);
        m_graph_version = cur;
        return;
    }

    // Reconcile the loaded/cached graph with the current data (add new, soft-delete gone).
    sync_in_memory(table);
    m_graph_version = cur;
}

bool VectorIndex::sync_in_memory(const Table& table)
{
    hnswlib::HierarchicalNSW<float>& hnsw = *m_graph->hnsw;
    const size_t dim = m_graph_dim;
    if (dim == 0)
        return false;

    std::vector<float> buf(dim);
    std::unordered_set<hnswlib::labeltype> current;
    bool changed = false;

    for (auto obj : table) {
        auto lst = obj.get_list<float>(m_column);
        if (lst.size() != dim)
            continue;
        hnswlib::labeltype key = hnswlib::labeltype(obj.get_key().value);
        current.insert(key);
        auto it = hnsw.label_lookup_.find(key);
        if (it == hnsw.label_lookup_.end()) {
            if (hnsw.cur_element_count >= hnsw.max_elements_)
                hnsw.resizeIndex(hnsw.max_elements_ * 2 + 16);
            for (size_t i = 0; i < dim; ++i)
                buf[i] = lst.get(i);
            if (m_config.metric == VectorMetric::Cosine)
                normalize_vec(buf.data(), dim);
            hnsw.addPoint(buf.data(), key);
            changed = true;
        }
        else if (hnsw.isMarkedDeleted(it->second)) {
            hnsw.unmarkDelete(key);
            changed = true;
        }
    }

    std::vector<hnswlib::labeltype> gone;
    for (const auto& kv : hnsw.label_lookup_) {
        if (!current.count(kv.first) && !hnsw.isMarkedDeleted(kv.second))
            gone.push_back(kv.first);
    }
    for (hnswlib::labeltype label : gone) {
        hnsw.markDelete(label);
        changed = true;
    }

    if (hnsw.num_deleted_ > 0 && hnsw.num_deleted_ * 2 >= hnsw.cur_element_count) {
        build_in_memory(table);
        changed = true;
    }
    return changed;
}

void VectorIndex::rebuild(const Table& table)
{
    std::lock_guard<std::mutex> lock(g_vector_index_mutex);
    build_in_memory(table);
    store_to_blob(table.size());
    m_graph_version = table.get_content_version();
    m_tried_load = true;
}

std::vector<ObjKey> VectorIndex::search(const Table& table, const std::vector<float>& query, size_t k,
                                        const std::unordered_set<uint64_t>& candidates)
{
    std::vector<ObjKey> out;
    if (query.empty() || candidates.empty())
        return out;

    std::lock_guard<std::mutex> lock(g_vector_index_mutex);
    ensure_graph(table);
    if (!m_graph || !m_graph->hnsw || m_graph->hnsw->cur_element_count == 0)
        return out;
    if (query.size() != m_graph_dim)
        throw IllegalOperation("Query vector dimension does not match the vector index");

    struct Filter : public hnswlib::BaseFilterFunctor {
        const std::unordered_set<uint64_t>& allowed;
        explicit Filter(const std::unordered_set<uint64_t>& a)
            : allowed(a)
        {
        }
        bool operator()(hnswlib::labeltype id) override
        {
            return allowed.find(uint64_t(id)) != allowed.end();
        }
    } filter(candidates);

    size_t kk = std::min({k, candidates.size(), size_t(m_graph->hnsw->cur_element_count)});
    if (kk == 0)
        return out;
    m_graph->hnsw->setEf(std::max({kk, candidates.size(), m_config.ef_search}));
    const float* qdata = query.data();
    std::vector<float> qnorm;
    if (m_config.metric == VectorMetric::Cosine) {
        qnorm = query;
        normalize_vec(qnorm.data(), qnorm.size());
        qdata = qnorm.data();
    }
    for (auto& hit : m_graph->hnsw->searchKnnCloserFirst(qdata, kk, &filter))
        out.push_back(ObjKey(int64_t(hit.second)));
    return out;
}
