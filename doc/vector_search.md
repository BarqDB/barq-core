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
cfg.ef_search = 128;                 // query-time beam width floor
table->add_vector_index(col, cfg);

table->remove_vector_index(col);     // drop it (queries fall back to brute force)
table->rebuild_vector_index(col);    // force a full re-index

// Query: restrict any view, then rank by closeness.
TableView v = table->where().less(col_price, 100).find_all();
v.knnsearch(col_embedding, query_vec, 10); // v now holds the 10 closest, closest first
```

Without an index, `knnsearch` still works (brute force / ephemeral graph).

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

- New and deleted objects are folded into the graph the next time a search runs
  inside a write transaction (or on `rebuild_vector_index`). Until then, read
  transactions stay exact by brute-forcing the few not-yet-absorbed keys and
  merging them into the result.
- Editing a vector in place is detected automatically: every `Lst<float>` write
  on an indexed column records the object in the index's pending list. The object
  is re-ranked from live data immediately and re-inserted into the graph at the
  next absorb.
- Tombstones from deletions compact via a full rebuild once they reach half the
  graph.

## Sync

The index is local by design. It is derived data: building, maintaining and
absorbing it writes **no replication instructions**, so it never appears in sync
changesets. Each device (or the server) builds and maintains its own index from
the synced vectors; two devices can even index the same column with different
metrics. `test_table.cpp: Table_VectorIndexLocalOnly` asserts this.
