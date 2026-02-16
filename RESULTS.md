# Query Condition Cache Extension for DuckDB — Design & Results

## Motivation

ClickHouse has a [Query Condition Cache](https://clickhouse.com/blog/clickhouse-query-condition-cache) that caches per-granule (8,192 rows) predicate evaluation results. On repeated queries with the same WHERE clause, entire granules can be skipped — saving both I/O and CPU.

We explored whether a similar optimization is feasible in DuckDB, implemented purely as an **extension** (no core modifications).

### Key Difference: ClickHouse vs DuckDB

| | ClickHouse | DuckDB |
|---|---|---|
| Storage unit | Granule (8,192 rows) | Row Group (122,880 rows) |
| Execution unit | Granule (8,192 rows) | Vector (2,048 rows) |
| Cache granularity | Granule = storage = execution | Vector (execution unit only) |
| I/O savings | Yes (skip reading granules from disk) | No (buffer pool caches at row group level) |
| CPU savings | Yes (skip decompression + predicate eval) | Partial (skip decompression at vector level) |

In ClickHouse, the granule is both the storage and execution unit, so skipping a granule saves I/O + CPU. In DuckDB, data is stored in row groups (122,880 rows) but executed in vectors (2,048 rows). The buffer pool caches entire row groups, so skipping a vector only saves **CPU** (decompression + predicate evaluation), not I/O.

## Architecture

```
condition_cache_build('t', 'x>5')       Normal query: SELECT * FROM t WHERE x>5
         |                                           |
         v                                           v
  Run SELECT rowid FROM t WHERE x>5        OptimizerExtension (post-optimize)
         |                                           |
         v                                           v
  Build bitvectors from rowids             Lookup cache by (table, filter_key)
  (bit per vector: has qualifying rows?)             |
         |                                           v
         v                                  If HIT: move original filters to
  Store in ObjectCache                      LogicalFilter above, inject cache
                                            filter as sole table_filter on scan
```

### File Structure

```
extension-template/src/
├── include/
│   ├── condition_cache_extension.hpp    # Extension class
│   ├── condition_cache_state.hpp        # Cache data structures (bitvectors, store)
│   ├── condition_cache_optimizer.hpp    # Optimizer extension
│   └── condition_cache_filter.hpp       # ROW_ID filter function
├── condition_cache_extension.cpp        # Entry point, registers functions + optimizer
├── condition_cache_state.cpp            # ObjectCache-based store (Lookup/Insert/Clear)
├── condition_cache_optimizer.cpp        # Plan walker + filter injection
├── condition_cache_filter.cpp           # Per-vector bitvector check (O(1))
└── condition_cache_functions.cpp        # build/clear/info table functions
```

## Cache Data Structure

Per-entry, we store a **bitvector per row group** where bit `i` = 1 means vector `i` within that row group has at least one qualifying row.

```
Row Group 0:  [1 0 0 0 1 0 0 0 ... 0]   (60 bits, one per vector)
Row Group 1:  [0 0 0 0 0 0 0 0 ... 0]   (all zeros = entire row group skippable)
Row Group 2:  [0 0 1 0 0 0 0 0 ... 0]
...
```

- 122,880 rows per row group / 2,048 rows per vector = 60 vectors per row group
- 60 bits fits in a single `uint64_t`
- Cache key: canonical serialization of `TableFilterSet` (e.g., `bench|pattern_val:pattern_val<50`)
- Storage: DuckDB's `ObjectCache` (per-database, non-evictable)

## Implementation: Two Approaches

### V1: Vector-Level Prototype (No Speedup)

**Approach**: Add cache filter alongside original table_filters.

```
SEQ_SCAN (bench)
  Filters:
    pattern_val < 50                  ← original (still here)
    __condition_cache_filter(rowid)   ← added cache filter
```

**Result**: No speedup. DuckDB's **adaptive filter** mechanism evaluates simple column filters before ExpressionFilters. The execution order becomes:

1. Decompress `pattern_val` column
2. Evaluate `pattern_val < 50` (most rows filtered out)
3. Cache filter runs on survivors only

The cache filter never gets a chance to skip decompression because the original filter already ran first.

**Evidence**: Debug counters showed the cache filter was called only 5,205 times out of 24,420 total vectors — it only ran on vectors that survived the original `pattern_val < 50` filter.

### V2: Filter Movement (22% Speedup)

**Approach**: Move original table_filters to a LogicalFilter node above the scan, leaving the cache filter as the **sole** table_filter.

```
HASH_AGGREGATE
  └── FILTER (pattern_val < 50)         ← original predicate moved here
        └── SEQ_SCAN (bench)
              Filters: __condition_cache_filter(rowid)  ← only filter on scan
```

**Key implementation details in `InjectCacheFilter()`**:

1. **Ensure filter-only columns are projected**: Columns used only in WHERE (not in SELECT) must be added to `projection_ids` so the LogicalFilter above can reference them.

2. **Convert table_filters to Expressions**: Each `TableFilter` is converted to an `Expression` via `ToExpression()` with a `BoundColumnRefExpression` referencing the LogicalGet's output. Critical subtlety: `LogicalGet::GetColumnBindings()` uses `projection_ids` **values** (not positions) as binding column indices.

3. **Replace table_filters with cache filter only**: Clear original filters, push `ExpressionFilter` on `ROW_ID` that checks the bitvector.

4. **Wrap with LogicalFilter**: Insert a `LogicalFilter` between the LogicalGet and its parent, containing the original predicate expressions.

**Why this works**: With the cache filter as the sole scan-level filter, DuckDB evaluates it first. When it returns `false` for a vector (no qualifying rows), `approved_tuple_count = 0`, and DuckDB calls `Skip()` on all other columns — **completely bypassing decompression**.

## Benchmark Results

**Setup**: 50M rows, 9 columns (id, pattern_val, 6x md5 hashes, 80-byte payload).
Filter: `pattern_val < 50` where `pattern_val = i % 60000`.
Selectivity: 847/24,420 vectors have matches (3.5%), 96.5% skippable.

### V2 Results (Filter Movement)

| Run | WITHOUT CACHE | WITH CACHE |
|-----|--------------|------------|
| 1   | 0.471s       | 0.101s     |
| 2   | 0.541s       | 0.075s     |
| 3   | 0.089s       | 0.065s     |
| 4   | 0.086s       | 0.062s     |
| 5   | 0.060s       | 0.055s     |

- **Cold state** (run 1): 0.471s → 0.101s = **4.7x speedup**
- **Warm state** (runs 3-5 avg): ~78ms → ~61ms = **~22% speedup**

The cold-state improvement is larger because first-run decompression cost is higher (CPU caches not yet warm). In warm state, DuckDB's buffer pool and CPU caching already make repeated queries fast, so the marginal improvement from skipping vector decompression is smaller.

### EXPLAIN Plan (With Cache)

```
┌───────────────────────────┐
│    UNGROUPED_AGGREGATE    │
│        count_star()       │
│          sum(#0)          │
└─────────────┬─────────────┘
┌─────────────┴─────────────┐
│         PROJECTION        │
│         length(h1)        │
└─────────────┬─────────────┘
┌─────────────┴─────────────┐
│           FILTER          │
│         (#1 < 50)         │        ← original predicate
└─────────────┬─────────────┘
┌─────────────┴─────────────┐
│         SEQ_SCAN          │
│        Table: bench       │
│        Projections:       │
│         h1, pattern_val   │
│          Filters:         │
│  __condition_cache_filter │
│          (rowid)          │        ← cache filter only
└───────────────────────────┘
```

## DuckDB Extension Hooks Used

| Hook | Purpose |
|------|---------|
| `OptimizerExtension` | Walk plan tree post-optimization, detect cache hits, inject filter |
| `ExpressionFilter` on `ROW_ID` | Per-vector filter evaluated during scan |
| `ObjectCache` | Per-database storage for cache entries |
| `TableFunction` | `condition_cache_build`, `condition_cache_clear`, `condition_cache_info` |

No core DuckDB modifications required. All functionality implemented through the extension API.

## Limitations

1. **No automatic population**: User must manually call `condition_cache_build()`.
2. **No auto-invalidation**: Cache is not invalidated on INSERT/UPDATE/DELETE. Must manually `condition_cache_clear()`.
3. **CPU savings only, no I/O savings**: DuckDB's buffer pool caches at row-group granularity. Skipping vectors only saves decompression CPU, not disk I/O.
4. **Filter key matching**: Uses canonical serialization of `TableFilterSet`. Complex or rewritten predicates may not match.
5. **No segment-level pre-decompression hook**: DuckDB's extension API does not expose a hook before column segment decompression. The filter-movement approach is a workaround — it moves original filters to a LogicalFilter above so the cache filter is the only scan filter, enabling DuckDB's own `Skip()` mechanism.

## Potential Improvements

1. **Automatic cache population**: Capture row IDs during query execution and build cache automatically on first run.
2. **Cache invalidation**: Hook into DuckDB's WAL or transaction commit to invalidate stale entries.
3. **Row-group level pruning**: Use `filter_prune_callback` on the `ScalarFunction` to skip entire row groups where no vectors have matches (currently not implemented due to API limitations in the extension build).
4. **Lower selectivity workloads**: The cache benefits increase with lower selectivity (fewer matching vectors = more skipping). At 0.1% selectivity, the speedup would be more dramatic.

## Git History

- Branch: `condition-cache-vector-level-prototype`
- V1 commit (vector-level, no speedup): `9673d8f`
- V2 changes (filter movement, 22% speedup): uncommitted
