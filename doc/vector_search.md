# Vector (semantic) search

Barq can index a list-of-floats property with an HNSW graph and answer
k-nearest-neighbour queries fast.

## API

```cpp
// Index a list-of-floats column (defaults: inner product, m=16, ef_construction=200).
table->add_vector_index(col);

// Or pick the metric and graph shape:
VectorIndexConfig cfg;
cfg.metric = VectorMetric::Cosine;   // InnerProduct | L2 | Cosine
cfg.m = 32;                          // graph out-degree
cfg.ef_construction = 400;           // build-time beam width
cfg.ef_search = 128;                 // query-time beam width floor. 0 (default) = auto:
                                     // widens with index size (64 up to 100k vectors,
                                     // 128 up to 1M, 192 up to 10M, 256 beyond) so
                                     // recall holds steady as the table grows
cfg.build_threads = 4;               // full-(re)build workers; 0 (default) = one per core
table->add_vector_index(col, cfg);

table->remove_vector_index(col);     // drop it (queries fall back to brute force)
table->rebuild_vector_index(col);    // force a full re-index

// Query: restrict any view, then rank by closeness.
TableView v = table->where().less(col_price, 100).find_all();
v.knnsearch(col_embedding, query_vec, 10); // v now holds the 10 closest, closest first

// Results are approximate (that is what makes them fast). The beam can be
// widened per query when one search needs better recall:
v.knnsearch(col_embedding, query_vec, 10, /*ef=*/256);
```

Measured on Yandex Deep1B (96-dim, real queries): at 100k vectors a 64-wide beam
answers in ~0.2 ms at 98.5% recall@10; ef=128 gives 99.6% at ~0.4 ms. Distance
math runs on SIMD (NEON on ARM, SSE on x86) and vectors are read from the file
in whole leaf chunks. A whole-table view skips candidate-set construction
entirely; filtered views pay one pass over the view's keys.

Without an index, `knnsearch` still works (brute force / ephemeral graph).

## How it is built

`add_vector_index` / `rebuild_vector_index` (and the first absorb after a bulk
load) assemble the graph in flat memory on all cores — hnswlib-style striped
locking, one lock per node — then append the finished arrays to the file in one
sequential pass. Building this way is ~20x faster than growing the persisted
copy-on-write arrays insert by insert (measured 21k vec/s at 100k, 96-dim). A
BFS-verified connectivity sweep afterwards re-links any node the concurrent
inserts left unreachable from the entry point. Steady-state maintenance
(a few new, deleted or edited objects per write) keeps updating the persisted
graph in place; a write that adds more vectors than the graph holds switches to
the bulk path automatically.

The transient build memory is roughly `N * (4*dim + 8*m + 30)` bytes — about
0.5 GB per million 96-dim vectors at m=16.

## How it is stored

The graph lives in the database file, decomposed into ordinary copy-on-write
arrays (header, id→key map, per-node levels, adjacency lists, the vector store,
and a pending list). Because it is regular barq storage:

- Nothing is loaded up front — searches page straight from the memory-mapped file.
- A write copies only the array leaves it touches, not the whole graph.
- Snapshot isolation is automatic: a reader pinned to an older transaction keeps
  searching the graph exactly as it was while a writer builds its own version.
- Searches on different transactions run in parallel; there is no process-wide lock.

## How it stays fresh

- Every data change is recorded the moment it happens: the table notifies the
  index of each object insert and erase, and every `Lst<float>` write on an
  indexed column records the object as edited. The keys queue up in small
  persisted event lists.
- The next search inside a write transaction (or `rebuild_vector_index`) folds
  the queued changes into the graph in O(changes) — point lookups only, never a
  table scan, so the first search after a write stays fast at any table size.
  Until then, read transactions stay exact by brute-forcing the queued keys and
  merging them into the result — also O(changes).
- `Table::clear` empties the graph wholesale.
- Safety valve: if a write burst queues more than a million events without a
  search, recording stops and the next absorb reconciles with one table diff,
  then re-arms. Pre-tracking (older) index files upgrade themselves the same
  way on their first absorb. Debug builds cross-check every event-driven absorb
  against a full table diff.
- Tombstones from deletions compact via a full rebuild once they reach half the
  graph.

## Sync

The index is local by design. It is derived data: building, maintaining and
absorbing it writes **no replication instructions**, so it never appears in sync
changesets. Each device (or the server) builds and maintains its own index from
the synced vectors; two devices can even index the same column with different
metrics. `test_table.cpp: Table_VectorIndexLocalOnly` asserts this.
