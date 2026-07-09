// Vector-search benchmark against real data (Yandex Deep1B subsets).
//
// Reads base vectors + query vectors from a sqlite-vec database (built by
// rubbish/build_deep1b_sqlite.py), loads N of them into a barq file, builds the
// HNSW index, then measures query latency and recall@k for a sweep of search
// beams — plus an exhaustive baseline. Ground truth for the subset is computed
// here by parallel brute force (the dataset's official ground truth is against
// the full 10M rows, so it only applies when all rows are loaded).
//
// usage: barq-benchmark-vector-search <deep1b.db> <n_vectors> [n_queries] [k] [barq_path]

#include <barq.hpp>
#include <barq/index_vector.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace barq;
using Clock = std::chrono::steady_clock;

namespace {

constexpr size_t DIM = 96;

double sec_since(Clock::time_point t0)
{
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

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

// ---- sqlite reading -------------------------------------------------------------

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

    // Base vectors live in sqlite-vec chunks: 1024 contiguous float32[96] rows per
    // blob, valid slots flagged in a bitmap. Rows were bulk-inserted in base-file
    // order, so vector i here is base row i (= ground-truth id i).
    {
        sqlite3_stmt* st = nullptr;
        const char* sql = "SELECT c.size, c.validity, v.vectors "
                          "FROM deep_vectors_chunks c "
                          "JOIN deep_vectors_vector_chunks00 v ON v.rowid = c.chunk_id "
                          "ORDER BY c.chunk_id";
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK)
            fail("prepare chunks");
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
        if (sqlite3_prepare_v2(db, "SELECT embedding FROM deep_queries ORDER BY rowid", -1, &st, nullptr) !=
            SQLITE_OK)
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

// ---- subset ground truth (parallel brute force) -----------------------------------

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

// ---- latency stats ----------------------------------------------------------------

struct Stats {
    double p50, p95, avg_ms, qps;
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
    s.qps = 1000.0 / s.avg_ms;
    return s;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: %s <deep1b.db> <n_vectors> [n_queries=500] [k=10] [barq_path=./bench_vec.barq] [sq8]\n",
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

    std::printf("== barq vector-search benchmark ==\n");
    std::printf("dataset: %s | N=%zu Q=%zu k=%zu dim=%zu metric=L2\n\n", sqlite_path, N, Q, K, DIM);

    // 1. Read data (probe mode still reads base vectors for ground truth)
    auto t0 = Clock::now();
    Dataset ds = read_dataset(sqlite_path, N ? N : 100000, Q);
    if (ds.n < N)
        std::printf("note: dataset holds only %zu vectors\n", ds.n);
    std::printf("[read]    %zu base + %zu query vectors in %.1fs\n", ds.n, ds.q, sec_since(t0));

    // 2. Subset ground truth
    t0 = Clock::now();
    auto gt = brute_force_gt(ds, K);
    double gt_time = sec_since(t0);
    std::printf("[gt]      brute-forced top-%zu for %zu queries in %.1fs (%.2f ms/query single-shot)\n", K, ds.q,
                gt_time, gt_time * 1000.0 / double(ds.q));

    // 3. Load into barq (probe mode: N==0 reuses an existing file, skips load+build)
    const bool probe = (N == 0);
    if (!probe) {
        util::File::try_remove(barq_path);
        util::File::try_remove(barq_path + ".lock");
    }
    DBRef sg = DB::create(barq_path);
    ColKey col_id, col_vec;
    if (probe) {
        ReadTransaction rt(sg);
        ConstTableRef t = rt.get_table("vectors");
        col_id = t->get_column_key("id");
        col_vec = t->get_column_key("embedding");
        int enc = t->has_vector_index(col_vec) ? int(t->get_vector_index(col_vec)->config().encoding) : -1;
        std::printf("[probe]   existing file, %zu objects, index=%d, encoding=%d\n", t->size(),
                    int(t->has_vector_index(col_vec)), enc);
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
        std::printf("[load]    %zu objects in %.1fs (%.0f obj/s), file %.1f MB\n", ds.n, load_time,
                    double(ds.n) / load_time, double(file_after_load) / 1e6);

    // The raw copy is no longer referenced (ground truth is done, the data lives
    // in barq now). Freeing it makes room for the build's in-RAM graph at 10M.
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
        wt.commit();
    }
    double build_time = sec_since(t0);
    size_t file_after_index = size_t(util::File(barq_path).get_size());
    if (!probe)
        std::printf("[index]   built in %.1fs (%.0f vec/s), file %.1f MB (+%.1f MB)\n\n", build_time,
                    double(ds.n) / build_time, double(file_after_index) / 1e6,
                    double(file_after_index - file_after_load) / 1e6);

    // 4b. Maintenance latency: the FIRST search after a small write must fold the
    // change in. A legacy (pre-event-tracking) file pays one table diff on the
    // first round and upgrades itself; the second round shows the steady state
    // (O(changes) event absorb). The read-txn round times the overlay path.
    {
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
            if (!contains(res, o.get_key()))
                std::printf("[resync]  WARNING: inserted object missing from its own result set\n");
            t->remove_object(o.get_key()); // leave the data unchanged...
            wt.commit();                   // ...but keep the (possibly upgraded) index
            std::printf("[resync]  %s: %.2f ms\n", label, ms);
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
            if (!contains(res, extra))
                std::printf("[resync]  WARNING: committed object missing from read-txn result set\n");
            std::printf("[resync]  read-txn first search after 1 committed insert (overlay): %.2f ms\n", ms);
        }
        {
            WriteTransaction wt(sg);
            TableRef t = wt.get_table("vectors");
            t->remove_object(extra);
            wt.commit();
        }
        std::printf("\n");
    }

    // 5. Query sweep
    {
        ReadTransaction rt(sg);
        ConstTableRef t = rt.get_table("vectors");

        // Index-only path: candidate set built once (the e2e number below includes
        // rebuilding it per query, which is what TableView::knnsearch does today).
        std::unordered_set<uint64_t> all_keys;
        all_keys.reserve(ds.n);
        std::unordered_map<uint64_t, int64_t> key_to_id; // ObjKey value -> base row
        key_to_id.reserve(ds.n);
        for (auto obj : *t) {
            all_keys.insert(uint64_t(obj.get_key().value));
            key_to_id[uint64_t(obj.get_key().value)] = obj.get<Int>(col_id);
        }
        VectorIndex* vindex = t->get_vector_index(col_vec);
        if (!vindex)
            fail("index missing after build");

        std::printf("%-12s %9s %9s %9s %9s %10s\n", "beam", "recall@k", "p50 ms", "p95 ms", "avg ms", "QPS");
        auto run_sweep = [&](size_t ef, const char* label, size_t n_queries = size_t(-1)) {
            size_t nq = std::min(ds.q, n_queries);
            std::vector<double> ms;
            ms.reserve(nq);
            double hits = 0, denom = 0;
            for (size_t qi = 0; qi < nq; ++qi) {
                std::vector<float> qv(ds.queries.data() + qi * DIM, ds.queries.data() + (qi + 1) * DIM);
                auto q0 = Clock::now();
                std::vector<ObjKey> res = vindex->search(*t, qv, K, all_keys, ef);
                ms.push_back(sec_since(q0) * 1000.0);
                std::unordered_set<int64_t> truth(gt[qi].begin(), gt[qi].end());
                for (ObjKey rk : res)
                    hits += truth.count(key_to_id[uint64_t(rk.value)]) ? 1 : 0;
                denom += double(truth.size());
            }
            Stats s = stats_from(ms);
            std::printf("%-12s %9.4f %9.3f %9.3f %9.3f %10.0f\n", label, hits / denom, s.p50, s.p95, s.avg_ms,
                        s.qps);
        };

        run_sweep(16, "ef=16");
        run_sweep(32, "ef=32");
        run_sweep(64, "ef=64");
        run_sweep(128, "ef=128");
        run_sweep(256, "ef=256");
        // The exhaustive beam costs ~n distance evaluations per query; cap the
        // query count at large n so this sanity row stays a few minutes.
        run_sweep(ds.n, "exhaustive", ds.n > 2'000'000 ? 20 : ds.q);

        // 6. e2e through the public API (find_all + knnsearch per query, default beam)
        {
            std::vector<double> ms;
            size_t reps = std::min<size_t>(ds.q, 100);
            for (size_t qi = 0; qi < reps; ++qi) {
                std::vector<float> qv(ds.queries.data() + qi * DIM, ds.queries.data() + (qi + 1) * DIM);
                auto q0 = Clock::now();
                TableView v = t->where().find_all();
                v.knnsearch(col_vec, qv, K);
                ms.push_back(sec_since(q0) * 1000.0);
                if (v.size() != K)
                    fail("e2e result size mismatch");
            }
            Stats s = stats_from(ms);
            std::printf("\n[e2e]     find_all+knnsearch (default beam, %zu queries): p50 %.2f ms, p95 %.2f ms "
                        "(candidate-set build dominates)\n",
                        reps, s.p50, s.p95);
        }
    }

    std::printf("\nbarq file kept at %s\n", barq_path.c_str());
    return 0;
}
