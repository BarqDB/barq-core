// Focused correctness check: barq's EXACT knn for a single query, compared
// id-for-id against the FAIR/Yandex official ground truth shipped in the
// Deep1B sqlite db. Opens the prebuilt barq file directly (no 10M reload).
//
// usage: barq-knn-check <deep1b.db> <barq_path> [query_id=1] [k=10]

#include <barq.hpp>
#include <barq/index_vector.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

using namespace barq;

static const size_t DIM = 96;

static float l2sq(const float* a, const float* b)
{
    double s = 0;
    for (size_t i = 0; i < DIM; ++i) {
        double d = double(a[i]) - double(b[i]);
        s += d * d;
    }
    return float(s);
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <deep1b.db> <barq_path> [query_id=1] [k=10]\n", argv[0]);
        return 1;
    }
    const char* sqlite_path = argv[1];
    const char* barq_path = argv[2];
    const int qid = argc > 3 ? std::atoi(argv[3]) : 1;
    const int k = argc > 4 ? std::atoi(argv[4]) : 10;

    // 1) query vector + official ground-truth ids from sqlite
    std::vector<float> qv;
    std::vector<int32_t> gt;
    {
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(sqlite_path, &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            std::fprintf(stderr, "cannot open sqlite db\n");
            return 1;
        }
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "SELECT embedding FROM deep_queries WHERE rowid=?", -1, &st, nullptr);
        sqlite3_bind_int64(st, 1, qid);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const float* p = static_cast<const float*>(sqlite3_column_blob(st, 0));
            qv.assign(p, p + DIM);
        }
        sqlite3_finalize(st);
        sqlite3_prepare_v2(db, "SELECT neighbor_ids FROM deep_groundtruth WHERE query_id=?", -1, &st, nullptr);
        sqlite3_bind_int64(st, 1, qid);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const int32_t* p = static_cast<const int32_t*>(sqlite3_column_blob(st, 0));
            int n = sqlite3_column_bytes(st, 0) / 4;
            gt.assign(p, p + std::min(n, k));
        }
        sqlite3_finalize(st);
        sqlite3_close(db);
    }
    if (qv.size() != DIM) {
        std::fprintf(stderr, "no query vector with rowid %d\n", qid);
        return 1;
    }

    // 2) barq EXACT knn — an ef >= live count routes to a true flat scan
    DBRef sg = DB::create(barq_path);
    ReadTransaction rt(sg);
    ConstTableRef t = rt.get_table("vectors");
    if (!t) {
        std::fprintf(stderr, "barq file has no 'vectors' table\n");
        return 1;
    }
    ColKey col_id = t->get_column_key("id");
    ColKey col_vec = t->get_column_key("embedding");
    VectorIndex* idx = t->get_vector_index(col_vec);
    if (!idx) {
        std::fprintf(stderr, "barq file has no vector index on 'embedding'\n");
        return 1;
    }
    std::vector<ObjKey> res = idx->search(*t, qv, size_t(k), nullptr, t->size());

    // Order our results nearest-first by exact L2 over the stored vectors.
    std::vector<std::pair<float, int64_t>> ours; // (L2^2, base id)
    std::vector<float> emb(DIM);
    for (ObjKey key : res) {
        if (!t->is_valid(key))
            continue;
        const Obj o = t->get_object(key);
        Lst<float> lst = o.get_list<float>(col_vec);
        float d = 0.0f;
        if (lst.size() == DIM) {
            for (size_t di = 0; di < DIM; ++di)
                emb[di] = lst.get(di);
            d = l2sq(qv.data(), emb.data());
        }
        ours.emplace_back(d, o.get<Int>(col_id));
    }
    std::sort(ours.begin(), ours.end());

    // 3) compare id-for-id
    std::printf("query_id %d — barq EXACT top-%d  vs  official ground truth (FAIR/Yandex)\n\n", qid, k);
    std::printf("  rank |   barq id     L2^2   | official id\n");
    int matches = 0;
    for (int i = 0; i < k; ++i) {
        int64_t bid = i < int(ours.size()) ? ours[i].second : -1;
        float bd = i < int(ours.size()) ? ours[i].first : 0.0f;
        int oid = i < int(gt.size()) ? gt[i] : -1;
        bool ok = bid == oid;
        matches += ok ? 1 : 0;
        std::printf("  %-4d | %9lld  %8.4f  | %-9d %s\n", i, (long long)bid, bd, oid, ok ? "OK" : "<-- differs");
    }
    std::printf("\nexact-id match vs official ground truth: %d/%d\n", matches, k);
    return matches == k ? 0 : 2;
}
