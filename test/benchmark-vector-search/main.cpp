// Vector-search benchmark against real data (Yandex Deep1B subsets).
//
// Reads base vectors + query vectors from a sqlite-vec database (built by
// rubbish/build_deep1b_sqlite.py), loads N of them into a barq file, builds the
// HNSW index, then measures query latency and recall@k for a sweep of search
// beams — plus an exact flat-scan baseline. Ground truth for the subset is
// computed here by parallel brute force (the dataset's official ground truth is
// against the full 10M rows, so it only applies when all rows are loaded).
//
// usage: barq-benchmark-vector-search <deep1b.db> <n_vectors> [n_queries] [k]
// [barq_path] [sq8]

#include <barq.hpp>
#include <barq/index_vector.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace barq;
using Clock = std::chrono::steady_clock;

namespace {

constexpr size_t DIM = 96;

double sec_since(Clock::time_point t0) { return std::chrono::duration<double>(Clock::now() - t0).count(); }

[[noreturn]] void fail(const std::string& msg)
{
    std::fprintf(stderr, "FATAL: %s\n", msg.c_str());
    std::exit(1);
}

float l2_sq(const float* a, const float* b)
{
    double s = 0;
    for (size_t i = 0; i < DIM; ++i) {
        double d = double(a[i]) - double(b[i]);
        s += d * d;
    }
    return float(s);
}

// Distances compared here are both produced by l2_sq() over the same stored
// float values. Allow only a small boundary tolerance for equal-distance ties.
bool within_l2_threshold(float distance, float threshold)
{
    if (!std::isfinite(distance) || !std::isfinite(threshold))
        return false;
    double scale = std::max(std::abs(double(threshold)), 1.0);
    double tolerance = 8.0 * double(std::numeric_limits<float>::epsilon()) * scale;
    return double(distance) <= double(threshold) + tolerance;
}

// The official blob can use a different float accumulation order. This looser
// check is only for validating blob decoding/alignment, never for recall.
bool official_distance_agrees(float local, float official)
{
    if (!std::isfinite(local) || !std::isfinite(official))
        return false;
    double scale = std::max({std::abs(double(local)), std::abs(double(official)), 1.0});
    double tolerance = 128.0 * double(std::numeric_limits<float>::epsilon()) * scale;
    return std::abs(double(local) - double(official)) <= tolerance;
}

void require_k_unique_valid(const Table& table, const std::vector<ObjKey>& results, size_t k,
                            const std::string& context)
{
    if (results.size() != k)
        fail(context + ": expected " + std::to_string(k) + " results, got " + std::to_string(results.size()));
    std::unordered_set<int64_t> seen;
    for (ObjKey key : results) {
        if (!table.is_valid(key))
            fail(context + ": returned an invalid ObjKey " + std::to_string(key.value));
        if (!seen.insert(key.value).second)
            fail(context + ": returned duplicate ObjKey " + std::to_string(key.value));
    }
}

// ---- sqlite reading
// -------------------------------------------------------------

struct Dataset {
    std::vector<float> base;    // n * DIM
    std::vector<float> queries; // q * DIM
    size_t n = 0;
    size_t q = 0;
};

Dataset read_dataset(const char* path, size_t want_n, size_t want_q)
{
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        fail("cannot open sqlite db");

    Dataset ds;
    ds.base.reserve(want_n * DIM);

    // Base vectors live in sqlite-vec chunks: 1024 contiguous float32[96] rows
    // per blob, valid slots flagged in a bitmap. Rows were bulk-inserted in
    // base-file order, so vector i here is base row i (= ground-truth id i).
    {
        sqlite3_stmt* st = nullptr;
        const char* sql = "SELECT c.size, c.validity, v.vectors "
                          "FROM deep_vectors_chunks c "
                          "JOIN deep_vectors_vector_chunks00 v ON v.rowid = c.chunk_id "
                          "ORDER BY c.chunk_id LIMIT ?";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
            fail("prepare chunks");
        sqlite3_bind_int64(st, 1, int64_t((want_n + 1023) / 1024));
        while (ds.base.size() < want_n * DIM && sqlite3_step(st) == SQLITE_ROW) {
            size_t size = size_t(sqlite3_column_int64(st, 0));
            const uint8_t* validity = static_cast<const uint8_t*>(sqlite3_column_blob(st, 1));
            const float* vecs = static_cast<const float*>(sqlite3_column_blob(st, 2));
            for (size_t i = 0; i < size && ds.base.size() < want_n * DIM; ++i) {
                if (!(validity[i / 8] & (1u << (i % 8))))
                    continue;
                ds.base.insert(ds.base.end(), vecs + i * DIM, vecs + (i + 1) * DIM);
            }
        }
        sqlite3_finalize(st);
    }
    ds.n = ds.base.size() / DIM;

    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT embedding FROM deep_queries ORDER BY rowid", -1, &st, nullptr) != SQLITE_OK)
            fail("prepare queries");
        while (ds.queries.size() < want_q * DIM && sqlite3_step(st) == SQLITE_ROW) {
            if (size_t(sqlite3_column_bytes(st, 0)) != DIM * sizeof(float))
                fail("query blob size mismatch");
            const float* qv = static_cast<const float*>(sqlite3_column_blob(st, 0));
            ds.queries.insert(ds.queries.end(), qv, qv + DIM);
        }
        sqlite3_finalize(st);
    }
    ds.q = ds.queries.size() / DIM;

    sqlite3_close(db);
    return ds;
}

// ---- subset ground truth (parallel brute force)
// -----------------------------------

std::vector<std::vector<int64_t>> brute_force_gt(const Dataset& ds, size_t k)
{
    std::vector<std::vector<int64_t>> gt(ds.q);
    size_t n_threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    std::atomic<size_t> next{0};
    for (size_t t = 0; t < n_threads; ++t) {
        pool.emplace_back([&] {
            for (size_t qi = next.fetch_add(1); qi < ds.q; qi = next.fetch_add(1)) {
                const float* qv = ds.queries.data() + qi * DIM;
                // max-heap of (dist, id), worst on top
                std::priority_queue<std::pair<float, int64_t>> heap;
                for (size_t i = 0; i < ds.n; ++i) {
                    float d = l2_sq(qv, ds.base.data() + i * DIM);
                    if (heap.size() < k)
                        heap.emplace(d, int64_t(i));
                    else if (d < heap.top().first) {
                        heap.pop();
                        heap.emplace(d, int64_t(i));
                    }
                }
                std::vector<int64_t> ids(heap.size());
                for (size_t j = ids.size(); j-- > 0;) {
                    ids[j] = heap.top().second;
                    heap.pop();
                }
                gt[qi] = std::move(ids);
            }
        });
    }
    for (auto& th : pool)
        th.join();
    return gt;
}

// ---- latency stats
// ----------------------------------------------------------------

struct Stats {
    double p50, p95, avg_ms, estimated_serial_qps;
};

Stats stats_from(std::vector<double>& ms)
{
    std::sort(ms.begin(), ms.end());
    double sum = 0;
    for (double v : ms)
        sum += v;
    Stats s;
    s.p50 = ms[ms.size() / 2];
    s.p95 = ms[size_t(double(ms.size()) * 0.95)];
    s.avg_ms = sum / double(ms.size());
    s.estimated_serial_qps = 1000.0 / s.avg_ms;
    return s;
}

// ---- official (independent) ground truth
// ------------------------------------------
//
// The Deep1B db ships FAIR/Yandex's official ground truth: for each query, the
// true nearest neighbors computed independently against the FULL base.
// `neighbor_ids` is a blob of little-endian int32 deep ids (0-based base row =
// our `id` column), and `distances` is the matching little-endian float32
// squared-L2 blob. Both are sorted nearest-first, `groundtruth_top_k` entries
// each. `query_id` is 1-based and equals our 0-based query index + 1. Comparing
// source brute force to this oracle validates source loading, L2, row-id
// alignment and GT decoding; index/search quality is measured separately below.

struct OfficialGT {
    std::vector<std::vector<int32_t>> per_query; // [q] -> nearest-first deep ids (empty if absent)
    std::vector<std::vector<float>> distances;   // [q] -> matching squared-L2 distances
    int64_t base_rows = 0;                       // base size the GT was computed against
    int top_k = 0;
};

OfficialGT read_official_gt(const char* path, size_t want_q)
{
    OfficialGT gt;
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        return gt;
    auto scalar = [&](const char* sql) -> int64_t {
        sqlite3_stmt* st = nullptr;
        int64_t v = 0;
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
            v = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        return v;
    };
    gt.base_rows = scalar("SELECT CAST(value AS INTEGER) FROM import_info WHERE key='base_rows'");
    gt.top_k = int(scalar("SELECT CAST(value AS INTEGER) FROM import_info WHERE "
                          "key='groundtruth_top_k'"));
    if (scalar("SELECT count(*) FROM deep_groundtruth") == 0) {
        sqlite3_close(db); // table absent or empty -> no official GT
        return gt;
    }
    gt.per_query.assign(want_q, {});
    gt.distances.assign(want_q, {});
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
                           "SELECT neighbor_ids, distances FROM deep_groundtruth "
                           "WHERE query_id = ?",
                           -1, &st, nullptr) == SQLITE_OK) {
        for (size_t qi = 0; qi < want_q; ++qi) {
            sqlite3_reset(st);
            sqlite3_clear_bindings(st);
            sqlite3_bind_int64(st, 1, int64_t(qi + 1)); // query_id is 1-based
            if (sqlite3_step(st) == SQLITE_ROW) {
                const void* ids_blob = sqlite3_column_blob(st, 0);
                const void* dist_blob = sqlite3_column_blob(st, 1);
                int ids_bytes = sqlite3_column_bytes(st, 0);
                int dist_bytes = sqlite3_column_bytes(st, 1);
                if (ids_blob && dist_blob && ids_bytes >= 4 && ids_bytes == dist_bytes && ids_bytes % 4 == 0) {
                    // The supported benchmark hosts (arm64/x86_64) are little-endian,
                    // matching the blobs, so raw copies preserve the stored bits.
                    size_t count = size_t(ids_bytes) / 4;
                    gt.per_query[qi].resize(count);
                    gt.distances[qi].resize(count);
                    std::memcpy(gt.per_query[qi].data(), ids_blob, ids_bytes);
                    std::memcpy(gt.distances[qi].data(), dist_blob, dist_bytes);
                }
            }
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return gt;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <deep1b.db> <n_vectors> [n_queries=500] [k=10] "
                     "[barq_path=./bench_vec.barq] [sq8]\n",
                     argv[0]);
        return 1;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* sqlite_path = argv[1];
    const size_t N = size_t(std::atoll(argv[2]));
    const size_t Q = argc > 3 ? size_t(std::atoll(argv[3])) : 500;
    const size_t K = argc > 4 ? size_t(std::atoll(argv[4])) : 10;
    const std::string barq_path = argc > 5 ? argv[5] : "./bench_vec.barq";
    const bool use_sq8 = argc > 6 && std::string(argv[6]) == "sq8";
    const bool probe = (N == 0);
    if (K == 0)
        fail("k must be greater than zero");

    std::printf("== barq vector-search benchmark ==\n");
    std::printf("dataset: %s | N=%zu Q=%zu k=%zu dim=%zu metric=L2\n\n", sqlite_path, N, Q, K, DIM);

    // Independent oracle: FAIR/Yandex official ground truth. It is only used for
    // recall when the loaded index covers the oracle's full base row count.
    OfficialGT ogt = read_official_gt(sqlite_path, Q);
    if (!ogt.per_query.empty())
        std::printf("[gt-off]  official ground truth present: top-%d, "
                    "base_rows=%lld (FAIR/Yandex GT_10M)\n",
                    ogt.top_k, static_cast<long long>(ogt.base_rows));
    else
        std::printf("[gt-off]  official ground truth: not present in this db\n");

    // Probe mode is read-only and follows the size of the existing index. This
    // also makes probing a 1M subset against a DB containing 10M official GT
    // valid (the official columns are simply disabled below).
    size_t probe_rows = 0;
    if (probe) {
        DBRef probe_db = DB::create(barq_path);
        ReadTransaction rt(probe_db);
        ConstTableRef table = rt.get_table("vectors");
        if (!table)
            fail("probe file has no 'vectors' table");
        ColKey probe_vec = table->get_column_key("embedding");
        if (!probe_vec || !table->get_vector_index(probe_vec))
            fail("probe file has no vector index on 'embedding'");
        probe_rows = table->size();
    }

    // 1. Read exactly the base rows represented by the new or reused index.
    auto t0 = Clock::now();
    size_t want_base = probe ? probe_rows : N;
    Dataset ds = read_dataset(sqlite_path, want_base, Q);
    if (ds.n < want_base)
        std::printf("note: dataset holds only %zu vectors\n", ds.n);
    if (ds.n < K)
        fail("benchmark needs at least k base vectors");
    if (ds.q == 0)
        fail("benchmark found no query vectors");
    std::printf("[read]    %zu base + %zu query vectors in %.1fs\n", ds.n, ds.q, sec_since(t0));

    // 2. Subset ground truth
    t0 = Clock::now();
    auto gt = brute_force_gt(ds, K);
    double gt_time = sec_since(t0);
    std::printf("[gt]      parallel flat top-%zu for %zu queries in %.1fs (%.2f "
                "ms/query wall/Q; not serial latency)\n",
                K, ds.q, gt_time, gt_time * 1000.0 / double(ds.q));

    // Official ground truth is only meaningful when the whole base is loaded (its
    // ids reference every base row). Gate the official-recall columns on a full
    // load.
    bool official_rows_ok = !ogt.per_query.empty() && ogt.distances.size() == ogt.per_query.size();
    if (official_rows_ok) {
        for (size_t qi = 0; qi < ds.q; ++qi) {
            if (ogt.per_query[qi].size() < K || ogt.distances[qi].size() < K) {
                official_rows_ok = false;
                break;
            }
        }
    }
    const bool official_ok =
        ogt.base_rows > 0 && int64_t(ds.n) == ogt.base_rows && K <= size_t(ogt.top_k) && official_rows_ok;
    if (!ogt.per_query.empty() && !official_ok)
        std::printf("[gt-off]  official recall disabled (loaded=%zu/%lld, "
                    "requested k=%zu, official top-k=%d, "
                    "complete id+distance data=%s)\n",
                    ds.n, static_cast<long long>(ogt.base_rows), K, ogt.top_k, official_rows_ok ? "yes" : "no");
    // Cross-check source brute force against the independent oracle two ways:
    // exact-id overlap (ties can choose different ids) and distance-threshold
    // agreement (any result no farther than the official kth distance is
    // correct). This check does not exercise barq's stored vectors, graph, SQ8,
    // or search path.
    if (official_ok) {
        double id_hits = 0, distance_hits = 0, denom = 0;
        size_t decoded_distance_mismatches = 0;
        for (size_t qi = 0; qi < ds.q; ++qi) {
            if (ogt.per_query[qi].size() < K || ogt.distances[qi].size() < K)
                continue;
            size_t topk = K;
            std::unordered_set<int64_t> off(ogt.per_query[qi].begin(), ogt.per_query[qi].begin() + topk);
            if (off.size() != topk)
                fail("official GT contains duplicate ids for query #" + std::to_string(qi));
            for (size_t rank = 0; rank < topk; ++rank) {
                int64_t id = ogt.per_query[qi][rank];
                if (id < 0 || size_t(id) >= ds.n)
                    fail("official GT id outside the loaded base for query #" + std::to_string(qi));
                if (!std::isfinite(ogt.distances[qi][rank]) || ogt.distances[qi][rank] < 0)
                    fail("official GT has an invalid L2 distance for query #" + std::to_string(qi));
            }
            const float* qv = ds.queries.data() + qi * DIM;
            int64_t threshold_id = ogt.per_query[qi][topk - 1];
            float threshold = l2_sq(qv, ds.base.data() + size_t(threshold_id) * DIM);
            if (!official_distance_agrees(threshold, ogt.distances[qi][topk - 1]))
                ++decoded_distance_mismatches;
            for (size_t rank = 0; rank < std::min(topk, gt[qi].size()); ++rank) {
                int64_t id = gt[qi][rank];
                id_hits += off.count(id) ? 1 : 0;
                if (id >= 0 && size_t(id) < ds.n &&
                    within_l2_threshold(l2_sq(qv, ds.base.data() + size_t(id) * DIM), threshold))
                    distance_hits += 1;
            }
            denom += double(topk);
        }
        std::printf("[verify]  source flat vs official top-%zu: exact-id %.4f, "
                    "distance-threshold %.4f\n",
                    K, denom ? id_hits / denom : 0.0, denom ? distance_hits / denom : 0.0);
        std::printf("[verify]  official kth-distance blob vs local L2: %zu/%zu "
                    "outside decode tolerance\n",
                    decoded_distance_mismatches, ds.q);
        if (decoded_distance_mismatches != 0)
            fail("official distance blobs do not match the decoded ids/query "
                 "alignment");
        std::printf("[verify]  scope: source base/query loader, L2, base-row ids, "
                    "and GT decode/alignment; "
                    "index/search is measured below\n");
    }

    // 3. Load into barq (probe mode: N==0 reuses an existing file, skips
    // load+build)
    if (!probe) {
        util::File::try_remove(barq_path);
        util::File::try_remove(barq_path + ".lock");
    }
    DBRef sg = DB::create(barq_path);
    ColKey col_id, col_vec;
    size_t persisted_ef_search = 0;
    if (probe) {
        ReadTransaction rt(sg);
        ConstTableRef t = rt.get_table("vectors");
        col_id = t->get_column_key("id");
        col_vec = t->get_column_key("embedding");
        VectorIndex* index = t->get_vector_index(col_vec);
        int enc = index ? int(index->config().encoding) : -1;
        persisted_ef_search = index ? index->config().ef_search : 0;
        std::printf("[probe]   existing file, %zu objects, index=%d, encoding=%d, "
                    "persisted ef_search=%zu%s\n",
                    t->size(), int(index != nullptr), enc, persisted_ef_search,
                    index && persisted_ef_search == 0 ? " (auto)" : "");
    }
    t0 = Clock::now();
    if (!probe) {
        const size_t batch = 20000;
        size_t written = 0;
        bool first = true;
        while (written < ds.n) {
            WriteTransaction wt(sg);
            TableRef t = first ? wt.add_table("vectors") : wt.get_table("vectors");
            if (first) {
                col_id = t->add_column(type_Int, "id");
                col_vec = t->add_column_list(type_Float, "embedding");
                first = false;
            }
            size_t stop = std::min(ds.n, written + batch);
            for (; written < stop; ++written) {
                Obj o = t->create_object();
                o.set(col_id, int64_t(written));
                Lst<float> lst = o.get_list<float>(col_vec);
                const float* v = ds.base.data() + written * DIM;
                for (size_t d = 0; d < DIM; ++d)
                    lst.add(v[d]);
            }
            wt.commit();
            std::printf("\r[load]    %zu/%zu", written, ds.n);
        }
        std::printf("\r");
    }
    double load_time = sec_since(t0);
    size_t file_after_load = size_t(util::File(barq_path).get_size());
    if (!probe)
        std::printf("[load]    %zu objects in %.1fs (%.0f obj/s), DB total %.1f MB\n", ds.n, load_time,
                    double(ds.n) / load_time, double(file_after_load) / 1e6);

    // Source-oracle work is complete and the vectors now live in Barq. Score all
    // search results against the actual stored vectors in both build and probe
    // modes, and release the 3.8 GB source copy before the index work.
    std::vector<float>().swap(ds.base);

    // 4. Build the index
    t0 = Clock::now();
    if (!probe) {
        WriteTransaction wt(sg);
        TableRef t = wt.get_table("vectors");
        VectorIndexConfig cfg;
        cfg.metric = VectorMetric::L2;
        cfg.m = 16;
        cfg.ef_construction = 200;
        cfg.ef_search = 64;
        if (use_sq8)
            cfg.encoding = VectorEncoding::SQ8;
        t->add_vector_index(col_vec, cfg);
        persisted_ef_search = t->get_vector_index(col_vec)->config().ef_search;
        wt.commit();
    }
    double build_time = sec_since(t0);
    size_t file_after_index = size_t(util::File(barq_path).get_size());
    if (!probe)
        std::printf("[index]   built in %.1fs (%.0f vec/s), index added %.1f MB, "
                    "DB total %.1f MB, "
                    "persisted ef_search=%zu%s\n\n",
                    build_time, double(ds.n) / build_time, double(file_after_index - file_after_load) / 1e6,
                    double(file_after_index) / 1e6, persisted_ef_search, persisted_ef_search == 0 ? " (auto)" : "");

    // Run maintenance only after quality/performance measurement so temporary
    // graph nodes cannot contaminate recall. Probe mode never calls this lambda
    // and remains read-only.
    auto run_maintenance = [&] {
        std::printf("[maint]   timings cover search() only; mutation setup, "
                    "cleanup and commit are excluded\n");
        std::vector<float> qv(ds.queries.data(), ds.queries.data() + DIM);
        auto contains = [](const std::vector<ObjKey>& res, ObjKey k) {
            return std::find(res.begin(), res.end(), k) != res.end();
        };
        auto timed_insert_search = [&](const char* label) {
            WriteTransaction wt(sg);
            TableRef t = wt.get_table("vectors");
            Obj o = t->create_object();
            o.set(col_id, int64_t(2'000'000'000));
            Lst<float> lst = o.get_list<float>(col_vec);
            for (size_t d = 0; d < DIM; ++d)
                lst.add(qv[d]);
            auto r0 = Clock::now();
            std::vector<ObjKey> res = t->get_vector_index(col_vec)->search(*t, qv, K, nullptr);
            double ms = sec_since(r0) * 1000.0;
            require_k_unique_valid(*t, res, K, label);
            if (!contains(res, o.get_key()))
                std::printf("[maint]   WARNING: inserted object missing from its own "
                            "result set\n");
            t->remove_object(o.get_key()); // leave the data unchanged...
            wt.commit();                   // ...but keep the (possibly upgraded) index
            std::printf("[maint]   %s [search only]: %.2f ms\n", label, ms);
        };
        timed_insert_search("write-txn first search after 1 insert (round 1, may scan+upgrade)");
        timed_insert_search("write-txn first search after 1 insert (round 2, event-tracked)");

        ObjKey extra;
        {
            WriteTransaction wt(sg);
            TableRef t = wt.get_table("vectors");
            Obj o = t->create_object();
            o.set(col_id, int64_t(2'000'000'001));
            Lst<float> lst = o.get_list<float>(col_vec);
            for (size_t d = 0; d < DIM; ++d)
                lst.add(qv[d]);
            extra = o.get_key();
            wt.commit(); // no search: change stays queued in the event lists
        }
        {
            ReadTransaction rt(sg);
            ConstTableRef t = rt.get_table("vectors");
            auto r0 = Clock::now();
            std::vector<ObjKey> res = t->get_vector_index(col_vec)->search(*t, qv, K, nullptr);
            double ms = sec_since(r0) * 1000.0;
            require_k_unique_valid(*t, res, K, "read-txn overlay search");
            if (!contains(res, extra))
                std::printf("[maint]   WARNING: committed object missing from read-txn "
                            "result set\n");
            std::printf("[maint]   read-txn first search after 1 committed insert "
                        "(overlay) [search only]: %.2f ms\n",
                        ms);
        }
        {
            WriteTransaction wt(sg);
            TableRef t = wt.get_table("vectors");
            t->remove_object(extra);
            wt.commit();
        }
        std::printf("\n");
    };

    // 5. Query sweep
    {
        ReadTransaction rt(sg);
        ConstTableRef t = rt.get_table("vectors");

        // Direct unfiltered path. A full-table TableView routes the same way:
        // nullptr candidates, avoiding a 10M-key bitmap. The public e2e rows below
        // additionally include view/filter construction and sort-descriptor
        // routing.
        std::unordered_map<uint64_t, int64_t> key_to_id; // ObjKey value -> base row
        key_to_id.reserve(ds.n);
        std::vector<ObjKey> id_to_key(ds.n);
        std::vector<uint8_t> id_seen(ds.n, 0);
        for (auto obj : *t) {
            int64_t id = obj.get<Int>(col_id);
            if (id < 0 || size_t(id) >= ds.n)
                fail("table row has a base id outside the loaded dataset");
            if (id_seen[size_t(id)])
                fail("table contains a duplicate base id " + std::to_string(id));
            id_seen[size_t(id)] = 1;
            id_to_key[size_t(id)] = obj.get_key();
            key_to_id[uint64_t(obj.get_key().value)] = id;
        }
        if (key_to_id.size() != ds.n)
            fail("barq table row count does not match the loaded base");
        VectorIndex* vindex = t->get_vector_index(col_vec);
        if (!vindex)
            fail("index missing after build");

        auto base_id_for = [&](ObjKey key) -> int64_t {
            auto it = key_to_id.find(uint64_t(key.value));
            if (it == key_to_id.end())
                fail("search returned an ObjKey with no base-id mapping: " + std::to_string(key.value));
            return it->second;
        };
        auto exact_distance_for_id = [&](const float* qv, int64_t id, std::vector<float>& scratch) {
            if (id < 0 || size_t(id) >= ds.n)
                fail("ground-truth id outside the loaded base: " + std::to_string(id));
            auto lst = t->get_object(id_to_key[size_t(id)]).get_list<float>(col_vec);
            if (lst.size() != DIM)
                fail("table vector dimension changed during benchmark");
            lst.get_tree().get_range(0, DIM, scratch.data());
            return l2_sq(qv, scratch.data());
        };

        struct WorstCase {
            bool valid = false;
            size_t query = 0;
            double distance_recall = 2.0;
            double id_recall = 2.0;
            std::vector<ObjKey> results;
        } worst_at_256;

        // Keep cold-start effects out of the beam comparison. This is only a
        // cache warm-up; its work is not included in any latency row below.
        size_t warmup_queries = std::min<size_t>(500, ds.q);
        for (size_t qi = 0; qi < warmup_queries; ++qi) {
            std::vector<float> qv(ds.queries.data() + qi * DIM, ds.queries.data() + (qi + 1) * DIM);
            std::vector<ObjKey> res = vindex->search(*t, qv, K, nullptr, 256);
            require_k_unique_valid(*t, res, K, "warm-up query #" + std::to_string(qi));
        }
        std::printf("[warmup]  %zu direct queries at ef=256 (not timed)\n", warmup_queries);

        // self-id = exact-id overlap with this benchmark's flat top-k.
        // off-id = exact-id overlap with FAIR/Yandex's top-k (ties can look wrong).
        // off-dist = returned items at/below the official kth squared-L2 threshold.
        std::printf("[search]  direct unfiltered search (candidates=nullptr); "
                    "excludes public view/filter construction\n");
        std::printf("%-12s %8s %8s %9s %8s %8s %8s %8s %8s %10s\n", "beam", "self-id", "off-id", "off-dist",
                    "dst-p5", "dst-min", "p50 ms", "p95 ms", "mean ms", "estQPS*");
        auto run_sweep = [&](size_t ef, const char* label, size_t n_queries = size_t(-1),
                             WorstCase* worst_out = nullptr) {
            size_t nq = std::min(ds.q, n_queries);
            std::vector<double> ms;
            ms.reserve(nq);
            double self_hits = 0, self_denom = 0;
            double off_id_hits = 0, off_distance_hits = 0, off_denom = 0;
            std::vector<double> distance_recalls;
            distance_recalls.reserve(nq);
            for (size_t qi = 0; qi < nq; ++qi) {
                std::vector<float> qv(ds.queries.data() + qi * DIM, ds.queries.data() + (qi + 1) * DIM);
                auto q0 = Clock::now();
                std::vector<ObjKey> res = vindex->search(*t, qv, K, nullptr, ef);
                ms.push_back(sec_since(q0) * 1000.0);
                require_k_unique_valid(*t, res, K, std::string(label) + " query #" + std::to_string(qi));
                std::unordered_set<int64_t> truth(gt[qi].begin(), gt[qi].end());
                for (ObjKey rk : res)
                    self_hits += truth.count(base_id_for(rk)) ? 1 : 0;
                self_denom += double(truth.size());
                if (official_ok && ogt.per_query[qi].size() >= K && ogt.distances[qi].size() >= K) {
                    size_t topk = K;
                    std::unordered_set<int64_t> otruth(ogt.per_query[qi].begin(), ogt.per_query[qi].begin() + topk);
                    double q_id_hits = 0, q_distance_hits = 0;
                    std::vector<float> exact(DIM);
                    float threshold = exact_distance_for_id(qv.data(), ogt.per_query[qi][topk - 1], exact);
                    for (ObjKey rk : res) {
                        int64_t id = base_id_for(rk);
                        q_id_hits += otruth.count(id) ? 1 : 0;
                        float distance = exact_distance_for_id(qv.data(), id, exact);
                        q_distance_hits += within_l2_threshold(distance, threshold) ? 1 : 0;
                    }
                    double id_recall = q_id_hits / double(topk);
                    double distance_recall = q_distance_hits / double(topk);
                    off_id_hits += q_id_hits;
                    off_distance_hits += q_distance_hits;
                    off_denom += double(topk);
                    distance_recalls.push_back(distance_recall);
                    if (worst_out &&
                        (!worst_out->valid || distance_recall < worst_out->distance_recall ||
                         (distance_recall == worst_out->distance_recall && id_recall < worst_out->id_recall))) {
                        worst_out->valid = true;
                        worst_out->query = qi;
                        worst_out->distance_recall = distance_recall;
                        worst_out->id_recall = id_recall;
                        worst_out->results = res;
                    }
                }
            }
            Stats s = stats_from(ms);
            if (official_ok && off_denom > 0 && !distance_recalls.empty()) {
                std::sort(distance_recalls.begin(), distance_recalls.end());
                double dmin = distance_recalls.front();
                double dp5 = distance_recalls[std::min(distance_recalls.size() - 1,
                                                       size_t(double(distance_recalls.size()) * 0.05))];
                std::printf("%-12s %8.4f %8.4f %9.4f %8.4f %8.4f %8.3f %8.3f %8.3f %10.1f\n", label,
                            self_hits / self_denom, off_id_hits / off_denom, off_distance_hits / off_denom, dp5, dmin,
                            s.p50, s.p95, s.avg_ms, s.estimated_serial_qps);
            } else {
                std::printf("%-12s %8.4f %8s %9s %8s %8s %8.3f %8.3f %8.3f %10.1f\n", label,
                            self_hits / self_denom, "--", "--", "--", "--", s.p50, s.p95, s.avg_ms,
                            s.estimated_serial_qps);
            }
        };

        run_sweep(16, "ef=16");
        run_sweep(32, "ef=32");
        run_sweep(64, "ef=64");
        run_sweep(128, "ef=128");
        run_sweep(256, "ef=256", size_t(-1), &worst_at_256);
        std::printf("[search]  * estimated serial QPS = 1000 / mean measured "
                    "latency; not concurrent throughput\n");

        // Diagnose the lowest distance-threshold recall, not the lowest exact-id
        // recall. Duplicate vectors with different ids are therefore not called
        // misses.
        if (official_ok && worst_at_256.valid) {
            const size_t ef = 256;
            size_t worst = worst_at_256.query;
            const float* wq = ds.queries.data() + worst * DIM;
            std::vector<float> exact(DIM);
            auto dist_to = [&](int64_t id) { return exact_distance_for_id(wq, id, exact); };
            size_t topk = K;
            float threshold = dist_to(ogt.per_query[worst][topk - 1]);
            std::vector<std::pair<float, int64_t>> ours; // (L2^2, id) nearest-first
            for (ObjKey rk : worst_at_256.results)
                ours.emplace_back(dist_to(base_id_for(rk)), base_id_for(rk));
            std::sort(ours.begin(), ours.end());
            std::printf("\n[worst]   query #%zu at ef=%zu: exact-id recall %.3f, "
                        "distance-threshold recall %.3f\n",
                        worst, ef, worst_at_256.id_recall, worst_at_256.distance_recall);
            std::printf("[worst]   official kth vector's local squared-L2 threshold %.9g "
                        "(blob %.9g; small tie tolerance applied)\n",
                        double(threshold), double(ogt.distances[worst][topk - 1]));
            std::printf("          %-4s | %-22s | %-22s | %-22s\n", "rank", "our search (id @ L2^2)",
                        "our brute force (id @ L2^2)", "official (id @ L2^2)");
            for (size_t r = 0; r < K; ++r) {
                char a[48], b[48], c[48];
                if (r < ours.size())
                    std::snprintf(a, sizeof a, "%9lld @ %10.4f", (long long)ours[r].second, ours[r].first);
                else
                    std::snprintf(a, sizeof a, "--");
                if (r < gt[worst].size())
                    std::snprintf(b, sizeof b, "%9lld @ %10.4f", (long long)gt[worst][r], dist_to(gt[worst][r]));
                else
                    std::snprintf(b, sizeof b, "--");
                if (r < ogt.per_query[worst].size())
                    std::snprintf(c, sizeof c, "%9lld @ %10.4f", (long long)ogt.per_query[worst][r],
                                  dist_to(ogt.per_query[worst][r]));
                else
                    std::snprintf(c, sizeof c, "--");
                std::printf("          %-4zu | %-22s | %-22s | %-22s\n", r, a, b, c);
            }
            size_t distance_hits = 0;
            for (auto& h : ours)
                distance_hits += within_l2_threshold(h.first, threshold) ? 1 : 0;
            if (distance_hits == topk)
                std::printf("[worst]   verdict: DISTANCE-EQUIVALENT -- all %zu results "
                            "are at or below the official kth vector's local "
                            "distance threshold\n",
                            topk);
            else
                std::printf("[worst]   verdict: MISS -- %zu of %zu returned vectors "
                            "are farther than the official "
                            "kth threshold\n",
                            topk - distance_hits, topk);
        }

        auto require_view_results = [&](const TableView& view, size_t expected, const std::string& context) {
            std::vector<ObjKey> keys;
            keys.reserve(view.size());
            for (size_t i = 0; i < view.size(); ++i)
                keys.push_back(view.get_object(i).get_key());
            require_k_unique_valid(*t, keys, expected, context);
        };

        // 6. Public full-table path: timer includes find_all(), sort-descriptor
        // routing and knnsearch(), unlike the direct search table above.
        {
            std::vector<double> ms;
            size_t reps = std::min<size_t>(ds.q, 100);
            for (size_t qi = 0; qi < reps; ++qi) {
                std::vector<float> qv(ds.queries.data() + qi * DIM, ds.queries.data() + (qi + 1) * DIM);
                auto q0 = Clock::now();
                TableView v = t->where().find_all();
                v.knnsearch(col_vec, qv, K);
                ms.push_back(sec_since(q0) * 1000.0);
                require_view_results(v, K, "public full-table query #" + std::to_string(qi));
            }
            Stats s = stats_from(ms);
            std::printf("\n[e2e-all] warm public find_all + routing + knnsearch "
                        "(persisted ef_search=%zu%s, %zu queries): "
                        "p50 %.2f ms, p95 %.2f ms\n",
                        persisted_ef_search, persisted_ef_search == 0 ? " auto" : "", reps, s.p50, s.p95);
        }

        // 6b. Public filtered path: timer includes predicate/filter scan, view
        // construction, candidate bitmap construction and filtered graph search.
        {
            std::vector<double> ms;
            size_t reps = std::min<size_t>(ds.q, 50);
            for (size_t qi = 0; qi < reps; ++qi) {
                std::vector<float> qv(ds.queries.data() + qi * DIM, ds.queries.data() + (qi + 1) * DIM);
                auto q0 = Clock::now();
                TableView v = t->where().less(col_id, int64_t(ds.n / 2)).find_all();
                v.knnsearch(col_vec, qv, K);
                ms.push_back(sec_since(q0) * 1000.0);
                require_view_results(v, std::min(K, ds.n / 2), "public half-table query #" + std::to_string(qi));
            }
            Stats s = stats_from(ms);
            std::printf("[e2e-half] warm public filter + n/2 view + "
                        "candidate bitmap + knnsearch "
                        "(persisted ef_search=%zu%s): "
                        "p50 %.2f ms, p95 %.2f ms\n",
                        persisted_ef_search, persisted_ef_search == 0 ? " auto" : "", s.p50, s.p95);
        }

        // Run the destructive-to-cache exact baseline last. search(ef >=
        // live_count) is routed by VectorIndex to a true table flat scan, not a
        // graph walk with a very wide beam.
        size_t flat_queries = ds.n > 2'000'000 ? std::min<size_t>(20, ds.q) : ds.q;
        std::printf("[flat]    exact table scan baseline over %zu/%zu queries "
                    "(requested with ef=N)\n",
                    flat_queries, ds.q);
        run_sweep(ds.n, "flat-exact", flat_queries);
    }

    // 7. Maintenance latency: the FIRST search after a small write must fold the
    // change in. A legacy (pre-event-tracking) file pays one table diff on the
    // first round and upgrades itself; the second round shows the steady state
    // (O(changes) event absorb). The read-txn round times the overlay path.
    if (probe)
        std::printf("[maint]   skipped (probe mode is read-only)\n");
    else
        run_maintenance();

    std::printf("\nbarq file kept at %s\n", barq_path.c_str());
    return 0;
}
