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

#include <barq/array_blob.hpp>
#include <barq/list.hpp>
#include <barq/table.hpp>

#include <algorithm>
#include <limits>
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

// Layout of the container (m_top, type_HasRefs):
constexpr size_t s_meta_slot = 0;   // -> Array(type_Normal) of int64 metadata
constexpr size_t s_chunks_slot = 1; // -> Array(type_HasRefs) of ArrayBlob refs

// Metadata array slots:
constexpr size_t s_meta_format = 0;
constexpr size_t s_meta_dim = 1;
constexpr size_t s_meta_count = 2; // number of vectors in the persisted graph
constexpr size_t s_meta_rows = 3;  // table row count when the graph was built (staleness check)

constexpr int64_t s_format_version = 1;
constexpr size_t s_chunk_bytes = 8 * 1024 * 1024; // keep each blob under the single-node blob cap

} // anonymous namespace

// The in-memory graph. Kept behind a PIMPL so hnswlib stays out of the header.
// m_space must outlive m_hnsw (the graph captures the space's distance-func param),
// so declaration order matters here.
struct VectorIndex::Graph {
    std::unique_ptr<hnswlib::InnerProductSpace> space;
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> hnsw;
};

VectorIndex::VectorIndex(ColKey column, Allocator& alloc)
    : m_column(column)
    , m_top(alloc)
{
    m_top.create(Array::type_HasRefs);

    Array meta(alloc);
    meta.create(Array::type_Normal);
    meta.add(s_format_version);
    meta.add(0); // dim
    meta.add(0); // count
    meta.add(0); // built content version
    m_top.add(from_ref(meta.get_ref()));

    Array chunks(alloc);
    chunks.create(Array::type_HasRefs);
    m_top.add(from_ref(chunks.get_ref()));
}

VectorIndex::VectorIndex(ref_type ref, ArrayParent* parent, size_t ndx_in_parent, ColKey column, Allocator& alloc)
    : m_column(column)
    , m_top(alloc)
{
    m_top.init_from_ref(ref);
    m_top.set_parent(parent, ndx_in_parent);
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
    m_top.init_from_parent();
    m_graph.reset();
    m_graph_version = uint64_t(-1);
    m_tried_load = false;
}

void VectorIndex::refresh_accessor_tree()
{
    m_top.init_from_parent();
    // The container may have advanced to a new version; drop the cached graph and
    // re-attempt loading the persisted blob on the next query.
    m_graph.reset();
    m_graph_version = uint64_t(-1);
    m_tried_load = false;
}

ref_type VectorIndex::get_ref() const noexcept
{
    return m_top.get_ref();
}

size_t VectorIndex::dim() const
{
    Array meta(m_top.get_alloc());
    meta.init_from_ref(m_top.get_as_ref(s_meta_slot));
    return size_t(meta.get(s_meta_dim));
}

size_t VectorIndex::count() const
{
    Array meta(m_top.get_alloc());
    meta.init_from_ref(m_top.get_as_ref(s_meta_slot));
    return size_t(meta.get(s_meta_count));
}

size_t VectorIndex::stored_row_count() const
{
    Array meta(m_top.get_alloc());
    meta.init_from_ref(m_top.get_as_ref(s_meta_slot));
    return size_t(meta.get(s_meta_rows));
}

void VectorIndex::build_in_memory(const Table& table)
{
    // Determine the vector dimension from the first non-empty list.
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
    g->space = std::make_unique<hnswlib::InnerProductSpace>(dim);
    size_t capacity = std::max<size_t>(table.size(), 1);
    g->hnsw = std::make_unique<hnswlib::HierarchicalNSW<float>>(g->space.get(), capacity, 16, 200);

    std::vector<float> buf(dim);
    for (auto obj : table) {
        auto lst = obj.get_list<float>(m_column);
        if (lst.size() != dim)
            continue; // only index vectors of the index's dimension
        for (size_t i = 0; i < dim; ++i)
            buf[i] = lst.get(i);
        g->hnsw->addPoint(buf.data(), hnswlib::labeltype(obj.get_key().value));
    }
    m_graph = std::move(g);
}

void VectorIndex::read_all_bytes(std::string& out) const
{
    Allocator& alloc = m_top.get_alloc();
    Array chunks(alloc);
    chunks.init_from_ref(m_top.get_as_ref(s_chunks_slot));
    for (size_t i = 0; i < chunks.size(); ++i) {
        ref_type r = chunks.get_as_ref(i);
        if (!r)
            continue;
        ArrayBlob blob(alloc);
        blob.init_from_ref(r);
        out.append(blob.get(0), blob.size());
    }
}

bool VectorIndex::load_from_blob()
{
    Allocator& alloc = m_top.get_alloc();
    Array meta(alloc);
    meta.init_from_ref(m_top.get_as_ref(s_meta_slot));
    size_t dim = size_t(meta.get(s_meta_dim));
    size_t count = size_t(meta.get(s_meta_count));
    if (dim == 0 || count == 0)
        return false;

    std::string bytes;
    read_all_bytes(bytes);
    if (bytes.empty())
        return false;

    std::istringstream iss(bytes, std::ios::binary);
    auto g = std::make_unique<Graph>();
    g->space = std::make_unique<hnswlib::InnerProductSpace>(dim);
    g->hnsw = std::make_unique<hnswlib::HierarchicalNSW<float>>(g->space.get());
    g->hnsw->loadIndex(iss, g->space.get());
    m_graph = std::move(g);
    m_graph_dim = dim;
    return true;
}

void VectorIndex::store_to_blob(size_t row_count)
{
    Allocator& alloc = m_top.get_alloc();

    std::string bytes;
    size_t count = 0;
    if (m_graph && m_graph->hnsw) {
        std::ostringstream oss(std::ios::binary);
        m_graph->hnsw->saveIndex(oss);
        bytes = oss.str();
        count = m_graph->hnsw->cur_element_count;
    }

    // Replace the chunk subtree wholesale.
    if (ref_type old_chunks = m_top.get_as_ref(s_chunks_slot))
        Array::destroy_deep(old_chunks, alloc);
    Array chunks(alloc);
    chunks.create(Array::type_HasRefs);
    size_t off = 0;
    while (off < bytes.size()) {
        size_t n = std::min(s_chunk_bytes, bytes.size() - off);
        ArrayBlob blob(alloc);
        blob.create();
        blob.add(bytes.data() + off, n);
        chunks.add(from_ref(blob.get_ref()));
        off += n;
    }
    m_top.set_as_ref(s_chunks_slot, chunks.get_ref());

    // Update metadata (COW propagates back through the parent chain).
    Array meta(alloc);
    meta.set_parent(&m_top, s_meta_slot);
    meta.init_from_parent();
    meta.set(s_meta_dim, int64_t(m_graph_dim));
    meta.set(s_meta_count, int64_t(count));
    meta.set(s_meta_rows, int64_t(row_count));
}

void VectorIndex::ensure_graph(const Table& table)
{
    uint64_t cur = table.get_content_version();
    if (m_graph && m_graph_version == cur)
        return;
    // On first use this session, try the persisted graph. It is valid as long as the
    // table's row count is unchanged since the graph was built — a cheap check that
    // catches inserts and deletes. (In-place vector edits keeping the same row count
    // require an explicit rebuild; incremental maintenance is a later stage.)
    if (!m_tried_load) {
        m_tried_load = true;
        if (count() > 0 && stored_row_count() == table.size() && load_from_blob()) {
            m_graph_version = cur;
            return;
        }
        m_graph.reset();
    }
    // Data changed within this session (or no valid persisted graph): rebuild in memory.
    build_in_memory(table);
    m_graph_version = cur;
}

void VectorIndex::rebuild(const Table& table)
{
    build_in_memory(table);
    store_to_blob(table.size());
    m_graph_version = table.get_content_version();
    m_tried_load = true; // the freshly built graph is already in memory
}

std::vector<ObjKey> VectorIndex::search(const Table& table, const std::vector<float>& query, size_t k,
                                        const std::unordered_set<uint64_t>& candidates)
{
    std::vector<ObjKey> out;
    if (query.empty() || candidates.empty())
        return out;

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
    m_graph->hnsw->setEf(std::max({kk, candidates.size(), size_t(64)}));
    for (auto& hit : m_graph->hnsw->searchKnnCloserFirst(query.data(), kk, &filter))
        out.push_back(ObjKey(int64_t(hit.second)));
    return out;
}
