# Query Condition Cache Extension for DuckDB — Design & Results

## Motivation

ClickHouse has a [Query Condition Cache](https://clickhouse.com/blog/introducing-the-clickhouse-query-condition-cache) that caches per-granule (8,192 rows) predicate evaluation results. On repeated queries with the same WHERE clause, entire granules can be skipped — saving both I/O and CPU.

We explored whether a similar optimization is feasible in DuckDB, implemented purely as an **extension** (no core modifications).

### Key Difference: ClickHouse vs DuckDB

| | ClickHouse | DuckDB |
|---|---|---|
| Storage unit | Part (immutable, contains granules) | Row Group (mutable via MVCC) |
| Execution unit | Granule (8,192 rows) | Vector (2,048 rows) |
| Cache granularity | Granule = storage = execution | Vector (execution unit only) |
| I/O savings | Yes (skip reading granules from disk) | No (buffer pool caches at row group level) |
| CPU savings | Yes (skip decompression + predicate eval) | Yes (skip decompression + predicate eval) |
| Invalidation | Free (immutable parts, part_name in key) | Requires validation (row group fingerprint) |

In ClickHouse, the granule is both the storage and execution unit, so skipping a granule saves I/O + CPU. In DuckDB, data is stored in row groups (122,880 rows) but executed in vectors (2,048 rows). The buffer pool caches entire row groups, so skipping a vector only saves **CPU** (decompression + predicate evaluation), not I/O.

### Cross-Column Alignment

Both ClickHouse and DuckDB guarantee that all columns within a storage unit (part/row group) have the **same number of rows**. Vectors are always aligned at the same offsets across all columns. This means our bitvector (one bit per vector) works correctly for any predicate, regardless of how many columns it touches.

```
Row Group 0 (122,880 rows):
  col1: [vec0][vec1][vec2]...[vec59]    <- 60 vectors
  col2: [vec0][vec1][vec2]...[vec59]    <- same 60 vectors, same row ranges
  col3: [vec0][vec1][vec2]...[vec59]    <- always aligned
```

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
         v                                  If HIT: inject cache filter on scan
  Store in ObjectCache                      (Path 1: move filters + inject,
                                             Path 2: inject alongside existing)
```

### File Structure

```
extension-template/src/
├── include/
│   ├── condition_cache_extension.hpp    # Extension class
│   ├── condition_cache_state.hpp        # Cache data structures (bitvectors, store)
│   ├── condition_cache_optimizer.hpp    # Optimizer extension
│   └── condition_cache_filter.hpp       # ROW_ID filter function + CacheExpressionFilter
├── condition_cache_extension.cpp        # Entry point, registers functions + optimizer
├── condition_cache_state.cpp            # ObjectCache-based store (Lookup/Insert/Clear)
├── condition_cache_optimizer.cpp        # Plan walker + filter injection (2 paths)
├── condition_cache_filter.cpp           # Per-vector bitvector check (O(1)) + row-group pruning
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
- Cache key: canonical serialization of filter (e.g., `events|event_text:(event_text ~~ '%ERROR_42%')`)
- Storage: DuckDB's `ObjectCache` (per-database, non-evictable)

## Optimizer Paths

The optimizer handles two paths depending on where DuckDB places the predicate:

### Path 1: Pushable Predicates (predicate in `table_filters`)

DuckDB pushes LIKE, JSON, AND predicates down to the scan's `table_filters` as `ExpressionFilter`. Our optimizer detects `LogicalGet` with `table_filters`, moves the expensive filters to a `LogicalFilter` above, and injects cache filter as the scan filter.

```
Original:                              With cache:
  SEQ_SCAN (events)                      FILTER (text ~~ '%ERROR%')     <- moved up
    table_filters:                         └── SEQ_SCAN (events)
      text ~~ '%ERROR%'                          table_filters:
                                                   cache_filter(rowid)  <- injected
```

**Zone map concern**: If the WHERE clause has both simple and expensive predicates (e.g., `col_int < 100 AND text LIKE '%ERROR%'`), Path 1 currently moves ALL filters up. This causes `col_int < 100` to lose zone map capability. The fix is to only move expensive filters (`ExpressionFilter`) and keep simple filters (`ConstantFilter`) in place:

```
Correct Path 1 for mixed predicates:
  FILTER (text ~~ '%ERROR%')                    <- only move expensive filter
    └── SEQ_SCAN (events)
          table_filters:
            col_int < 100                       <- keep! zone map still works
            cache_filter(rowid)                 <- cache added alongside
```

### Path 2: Non-Pushable Predicates (predicate in `LogicalFilter`)

DuckDB cannot push OR predicates down. They stay in `LogicalFilter` above the scan. Our optimizer detects the `LogicalFilter -> LogicalGet` pattern and injects cache filter on the scan without moving anything.

```
Original:                              With cache:
  FILTER (A OR B)                        FILTER (A OR B)              <- unchanged
    └── SEQ_SCAN (events)                  └── SEQ_SCAN (events)
          table_filters: []                      table_filters:
                                                   cache_filter(rowid) <- injected
```

No zone map interference — OR couldn't use zone maps anyway, and any existing `table_filters` remain untouched.

### Row-Group Level Pruning

`CacheExpressionFilter` subclass overrides `CheckStatistics()` to skip entire row groups where all vectors have `mask = 0`. This avoids even loading the row group from the buffer pool.

## Benchmark Results

All benchmarks run on the same machine with warm buffer pool (unless noted).

### Test 1: Single LIKE Predicate (30M rows)

**Setup**: 30M rows, 10 columns (1 text for LIKE, 6x md5 hashes, 2x payload).
Filter: `event_text LIKE '%ERROR_42%'`, match every 50,000th row.
Selectivity: 600 / 14,640 vectors have matches (4%), 96% skippable.

| Run | WITHOUT CACHE | WITH CACHE |
|-----|--------------|------------|
| 1   | 0.583s       | 0.094s     |
| 2   | 0.502s       | 0.087s     |
| 3   | 0.404s       | 0.157s     |
| 4   | 0.540s       | 0.044s     |
| 5   | 0.533s       | 0.033s     |
| **Avg** | **0.512s** | **0.083s** |

**Speedup: 6.2x wall time / 11.6x CPU time**

```
EXPLAIN:
  FILTER (#8 ~~ '%ERROR_42%')              <- correctness check
    └── SEQ_SCAN (events)
          Filters: __condition_cache_filter(rowid)  <- cache skips 96% of vectors
```

**What's saved**: For each skipped vector, we avoid:
1. `event_text` string column decompression (FSST/dictionary decode)
2. `contains()` string matching (per-byte scan)
3. All 8 output columns decompression

### Test 2: JSON Extract Predicate (30M rows)

**Setup**: 30M rows, 10 columns (1 JSON data column, 6x md5 hashes, 2x payload).
Filter: `json_extract_string(data, '$.type') = 'error'`, match every 50,000th row.
Same selectivity: 600 / 14,640 vectors (4%), 96% skippable.

| Run | WITHOUT CACHE | WITH CACHE |
|-----|--------------|------------|
| 1   | 0.426s       | 0.067s     |
| 2   | 0.417s       | 0.077s     |
| 3   | 0.360s       | 0.071s     |
| 4   | 0.364s       | 0.055s     |
| 5   | 0.449s       | 0.050s     |
| **Avg** | **0.403s** | **0.064s** |

**Speedup: 6.3x wall time / 10.1x CPU time**

**What's saved**: For each skipped vector, we avoid:
1. `data` string column decompression
2. JSON parsing + field extraction (`json_extract_string`)
3. All 8 output columns decompression

### Test 3: Compound AND — Two LIKE Predicates (30M rows)

**Setup**: 30M rows, 8 columns (2 text for LIKE, 4x md5 hashes, 2x payload).
Filter: `text_a LIKE '%ERROR%' AND text_b LIKE '%WARN%'`.
- text_a matches every 50,000th row (600 matches)
- text_b matches every 40,000th row (750 matches)
- AND: both match every 200,000th row (150 matches)
- Selectivity: 150 / 9,000 vectors (1.7%), 98.3% skippable.

| Run | WITHOUT CACHE | WITH CACHE |
|-----|--------------|------------|
| 1   | 0.078s       | 0.022s     |
| 2   | 0.076s       | 0.032s     |
| 3   | 0.071s       | 0.015s     |
| 4   | 0.082s       | 0.007s     |
| 5   | 0.068s       | 0.007s     |
| **Avg** | **0.075s** | **0.017s** |

**Speedup: 4.4x**

Without cache is already fast (0.075s vs single LIKE's 0.512s) because DuckDB's **adaptive filter** short-circuits: the first LIKE eliminates 96% of vectors, so the second LIKE and output columns only decompress for the surviving 4%. Cache still helps by skipping the first LIKE's string decompression + evaluation.

### Test 4: Compound OR — Two LIKE Predicates (30M rows)

**Setup**: Same table as Test 3.
Filter: `text_a LIKE '%ERROR%' OR text_b LIKE '%WARN%'`.
- OR selectivity: ~1,200 / 14,640 vectors (8.2%), 92% skippable.
- **Critical**: OR predicates **cannot be pushed down** to table_filters in DuckDB. The entire OR expression stays in `LogicalFilter` above the scan -> DuckDB must decompress **ALL columns for ALL vectors**.

| Run | WITHOUT CACHE | WITH CACHE |
|-----|--------------|------------|
| 1   | 2.593s       | 0.163s     |
| 2   | 2.640s       | 0.152s     |
| 3   | 2.640s       | 0.151s     |
| 4   | 2.594s       | 0.144s     |
| 5   | 2.572s       | 0.105s     |
| **Avg** | **2.608s** | **0.143s** |

**Speedup: 18.2x**

This is the biggest win. Without cache, DuckDB cannot optimize OR at all:

```
Without cache:                          With cache (Path 2):
  LogicalFilter(A OR B)                   LogicalFilter(A OR B)
    └── SEQ_SCAN                            └── SEQ_SCAN
          -> decompress ALL columns                Filters: cache_filter(rowid)
            for ALL 14,640 vectors                -> skip 92% of vectors
          -> then evaluate OR                      -> only 1,200 vectors processed
```

For AND, DuckDB's adaptive filter can short-circuit (first LIKE fails -> skip everything). For OR, **both predicates must be evaluated** even if one fails — no short-circuit possible.

### Test 5: Simple Integer OR (30M rows)

**Setup**: 30M rows, 8 columns (2 integer filters, 4x md5 hashes, 2x payload).
Filter: `val_a < 50 OR val_b < 30` where `val_a = i % 60000`, `val_b = i % 80000`.
Selectivity: 758 / 14,640 vectors (5.2%), 95% skippable.
**Critical**: Even though both predicates are simple integer comparisons, OR cannot be pushed down.

| Run | WITHOUT CACHE | WITH CACHE |
|-----|--------------|------------|
| 1   | 1.877s       | 0.015s     |
| 2   | 1.907s       | 0.013s     |
| 3   | 1.864s       | 0.013s     |
| 4   | 1.824s       | 0.012s     |
| 5   | 1.969s       | 0.012s     |
| **Avg** | **1.888s** | **0.013s** |

**Speedup: 145x**

The most extreme result. Without cache, DuckDB has no optimization for OR — ALL 8 columns (including md5 strings and payloads) are decompressed for ALL 14,640 vectors even though the integer comparison itself is trivial. The 1.9s is almost entirely spent on output column decompression. Cache skips 95% of vectors, eliminating that decompression entirely.

### Test 6: Simple Integer Predicate (50M rows)

**Setup**: 50M rows, 9 columns (1 integer filter, 6x md5 hashes, 1 payload).
Filter: `pattern_val < 50` where `pattern_val = i % 60000`.
Selectivity: 847 / 24,420 vectors (3.5%), 96.5% skippable.

| Run | WITHOUT CACHE | WITH CACHE |
|-----|--------------|------------|
| 1 (cold) | 0.471s | 0.101s |
| 3 (warm) | 0.089s | 0.065s |
| 5 (warm) | 0.060s | 0.055s |

**Cold speedup: 4.7x / Warm speedup: ~22%**

Limited benefit because integer column decompression + comparison is already very cheap. DuckDB's native pushdown already skips output columns for non-matching vectors.

### Test 7: Mixed AND — Integer + LIKE (30M rows)

**Setup**: 30M rows, 8 columns (1 integer, 1 text for LIKE, 4x md5 hashes, 2x payload).
Filter: `val < 50 AND text_col LIKE '%ERROR%'`.
- `val < 50` matches 0.08% of rows (50/60000)
- `LIKE '%ERROR%'` matches every 50,000th row
- AND: 100 / 6,000 vectors have matches (1.7%), 98.3% skippable.

| Run | WITHOUT CACHE | WITH CACHE |
|-----|--------------|------------|
| 1   | 0.056s       | 0.005s     |
| 2   | 0.023s       | 0.005s     |
| 3   | 0.036s       | 0.006s     |
| 4   | 0.020s       | 0.005s     |
| 5   | 0.012s       | 0.008s     |
| **Avg** | **0.029s** | **0.006s** |

**Speedup: ~5x**

This test validates the **selective filter movement** optimization. The EXPLAIN shows the correct plan:

```
FILTER (contains(text_col, 'ERROR'))         <- only LIKE moved up (correctness)
  └── SEQ_SCAN (events)
        Filters:
          val<50                             <- ConstantFilter preserved! zone map works
          __condition_cache_filter(rowid)    <- cache filter added alongside
```

Three layers working together:
1. **Zone map**: `val < 50` skips entire row groups (preserved, zero overhead)
2. **Cache**: bitvector skips vectors where no rows match the full predicate
3. **FILTER**: `contains(text_col, 'ERROR')` runs only on surviving vectors

Without cache is already fast (0.029s) because DuckDB's adaptive filter runs `val < 50` first, eliminating most vectors. Cache adds value by also encoding the LIKE result — skipping LIKE evaluation + output column decompression for the remaining vectors.

### Summary: When Does the Cache Help?

| Predicate Type | Filter Cost | DuckDB Native | Cache Benefit | Evidence |
|---|---|---|---|---|
| `col < 100` (integer) | Very low | Zone maps + pushdown | Minimal (~22%) | Test 6 |
| `LIKE '%pattern%'` | **High** | Pushdown as ExpressionFilter | **6.2x** | Test 1 |
| `json_extract(...)` | **Very high** | Pushdown as ExpressionFilter | **6.3x** | Test 2 |
| `LIKE A AND LIKE B` | High | Adaptive filter short-circuits | **4.4x** | Test 3 |
| **`LIKE A OR LIKE B`** | **Very high** | **Cannot push down at all** | **18.2x** | Test 4 |
| **`int A OR int B`** | **Very low** | **Cannot push down (OR)** | **145x** | Test 5 |
| `int AND LIKE` (mixed) | Medium | Zone map + adaptive filter | **5x** | Test 7 |

**Rule of thumb**: The less DuckDB can natively optimize a predicate, the more the cache helps.
- Simple predicates (AND): zone maps + adaptive filter already handle well -> minimal benefit
- Expensive single predicates (LIKE, JSON): cache skips string decompression + evaluation -> 6x
- OR predicates: DuckDB has **no optimization** -> cache provides the biggest win -> **18-145x**
- Mixed AND (simple + expensive): zone map preserved + cache adds value -> 5x

## Zone Map Interaction

The cache and zone maps operate at **different layers** and are **complementary**:

```
Layer         Granularity    Applicable Predicates      Mechanism
-----------------------------------------------------------------------
Zone Map      Row group      col < 100, col = x         min/max statistics
Cache         Vector         LIKE, JSON, OR, regex      bitvector
Adaptive      Row (vector)   All                        runtime short-circuit
```

```
Query comes in
  |
  +-- Simple predicates (=, <, >, IN)
  |     └── Zone maps skip row groups <- native, unaffected
  |
  └── Expensive predicates (LIKE, OR, JSON, regex)
        └── Zone maps can't help <- cache fills this gap
              └── Cache skips vectors (92-98% skip)
```

**Path 2 (non-pushable predicates like OR)**: Cache filter is **added alongside** existing table_filters. Zone maps continue working on any pushable filters. No interference.

**Path 1 (pushable predicates)**: Must be careful with mixed predicates. Only move expensive filters (`ExpressionFilter`) to `LogicalFilter` above; keep simple filters (`ConstantFilter`) in `table_filters` so zone maps still work. `CacheExpressionFilter::CheckStatistics()` provides equivalent row-group level pruning for the cached predicate.

**Auto-caching guideline**: Only cache expensive predicates (LIKE, JSON, OR, regex). Simple predicates should be left to zone maps — they're already optimal, require no build cost, and automatically handle data changes.

## ClickHouse Source Code Analysis

### Cache Key Design

From [QueryConditionCache.h](https://github.com/ClickHouse/ClickHouse/blob/625c38213fe9276a091873e25eff852ae504f0d5/src/Interpreters/Cache/QueryConditionCache.h):

```cpp
// ClickHouse cache key = SipHash(table_uuid + part_name + condition_hash)
static Key makeKey(const UUID & table_id, const String & part_name, UInt64 condition_hash);
```

The key insight: ClickHouse includes `part_name` in the cache key. Since MergeTree parts are **immutable**, any data modification creates a new part with a new name. The old cache entry becomes orphaned and is evicted by LRU. **Invalidation is free** — no hooks, no callbacks, no validation needed.

```
INSERT -> new part (new name) -> cache miss -> rebuild on first scan
DELETE -> mutation creates new part -> old entry orphaned -> LRU evict
UPDATE -> mutation creates new part -> same as delete
MERGE  -> old parts merged into new part -> old entries all orphaned
```

### Cache Population

ClickHouse writes to the cache **during query execution** — each processing stream records which granules matched the predicate as it scans them. Zero overhead on first query. The cache entry is initialized with all marks set to `true` (assume all match), then progressively set to `false` as scan confirms no matches:

```cpp
// Entry initialized: all marks potentially match
QueryConditionCache::Entry::Entry(size_t mark_count)
    : matching_marks(mark_count, true) {}

// During scan: mark non-matching ranges as false
void QueryConditionCache::write(..., const MarkRanges & mark_ranges, ...) {
    for (const auto & mark_range : mark_ranges)
        std::fill(entry->matching_marks.begin() + mark_range.begin,
                  entry->matching_marks.begin() + mark_range.end, false);
}
```

### Comparison

| | ClickHouse | DuckDB (ours) |
|---|---|---|
| Cache key | `hash(table_uuid, part_name, condition_hash)` | `hash(table_name, condition_string)` |
| Population | During execution (zero overhead) | Separate build query (1x overhead) |
| Invalidation | Free (immutable parts) | Needs validation (fingerprint) |
| Eviction | LRU by size (configurable, default 100MB) | None (ObjectCache, non-evictable) |
| Predicate reuse | Same predicate across different queries | Same predicate across different queries |

## Cache Invalidation Design

### Problem

DuckDB's row groups are mutable (via MVCC). Unlike ClickHouse's immutable parts, there's no natural identifier that changes when data is modified. We need explicit validation.

### Available DuckDB Extension Hooks

| Hook | Can Detect Data Modification? | Granularity |
|------|-------------------------------|-------------|
| `StorageExtension::OnCheckpointEnd` | Indirectly (checkpoint time) | Entire DB |
| WAL `WriteInsert/Delete/Update` | Yes, but **not exposed** to extensions | Per-operation + table |
| `CatalogEntry::OnDrop/CommitAlter` | DDL only, not DML | Per-table |
| `ExtensionCallback` | No | Connection level |

**Conclusion**: No per-table data modification hook exists in the extension API.

### Solution: Row Group Metadata Fingerprint

Store a fingerprint of the table's row group structure at cache build time. Validate at query time (in optimizer, before using cache entry).

```
Build time:                          Query time (optimizer):
  Compute fingerprint:                 Recompute fingerprint:
  - num_row_groups                     - Compare with stored value
  - per row group: (start, count)      |
  - total_row_count                    +-- Match -> use cache
  → hash all into uint64_t             +-- Mismatch -> invalidate, skip cache
  → store with cache entry
```

**Cost**: O(num_row_groups) metadata scan. For 30M rows ~ 245 row groups -> microseconds.

**Coverage**:

| Operation | Detected? | Why |
|-----------|-----------|-----|
| INSERT | Yes | Row count / row group count changes |
| DELETE | Yes | Row count changes, compaction changes structure |
| UPDATE | Yes | DuckDB does DELETE + INSERT internally (MVCC) |
| CHECKPOINT | Yes | Row group reorganization changes structure |
| DELETE N + INSERT N (same count) | Usually | Row group structure typically changes |

**DuckDB-adapted key** (inspired by ClickHouse's `part_name` approach):

```
ClickHouse: Key = hash(table_uuid, part_name,      condition_hash)
                                    ^-- free, changes with data

DuckDB:     Key = hash(table_name,  rg_fingerprint, condition_hash)
                                    ^-- O(n) but microsecond-level
```

## Real-World Workload Scenarios

### Dashboard / BI Tools (Metabase, Superset, Grafana)

Same WHERE clause, different SELECT/GROUP BY as users switch dimensions:

```sql
-- Same predicate, different aggregations -> cache reuse
SELECT region, sum(revenue) FROM sales
  WHERE status LIKE '%error%' AND payload->>'type' = 'refund' GROUP BY region;
SELECT product, count(*) FROM sales
  WHERE status LIKE '%error%' AND payload->>'type' = 'refund' GROUP BY product;
```

This is exactly the pattern ClickHouse's blog demonstrates with the pretzel emoji queries.

### Log / Observability Analysis

Repeated drill-down on the same filter across GB of logs:

```sql
SELECT * FROM logs WHERE message LIKE '%OutOfMemory%';
SELECT timestamp, service FROM logs WHERE message LIKE '%OutOfMemory%' LIMIT 100;
SELECT count(*) FROM logs WHERE message LIKE '%OutOfMemory%' GROUP BY hour(timestamp);
```

### JSON Data Exploration

DuckDB is commonly used to analyze Parquet/JSON files:

```sql
SELECT * FROM events WHERE json_extract(data, '$.error.code') = '500';
-- Change SELECT, keep WHERE -> cache hit
```

### DuckDB's Natural Advantage

DuckDB is often used in **file-based** scenarios (Parquet, CSV) where data is **immutable** — loaded once, queried many times. Cache invalidation is rarely a concern in these use cases.

## Auto-Population Design

### Current State (Manual)

User must explicitly call `condition_cache_build('table', 'predicate')` before queries can benefit. This is functional but not user-friendly.

### Target State (Semi-Automatic, like ClickHouse)

Enable via setting, cache populated automatically on first query, reused on subsequent queries.

### Approach: Inline Build in Optimizer

When the optimizer detects an expensive predicate with no cache entry, build the cache inline before injecting the filter:

```
Query 1 (no cache):
  Optimizer:
    1. Detect expensive predicate (LIKE, JSON, OR, regex)
    2. Lookup cache -> miss
    3. Run SELECT rowid FROM table WHERE predicate  <- inline build
    4. Build bitvectors, store in cache
    5. Inject cache filter into plan
    -> Execute query (with cache)

Query 2+ (cache hit):
  Optimizer:
    1. Detect same predicate
    2. Lookup cache -> hit (+ validate fingerprint)
    3. Inject cache filter into plan
    -> Execute query (with cache)
```

**First query overhead**: Build cost ~ 1x normal execution. Total first query ~ same as without cache. Subsequent queries: 6-18x faster.

**Recursion prevention**: The internal build query would trigger the same optimizer. Prevented by existing `tl_build_capture` thread-local flag — when `active = true`, optimizer captures the filter key instead of building.

**Expensive predicate detection**: Only auto-build for predicates that benefit:
- `ExpressionFilter` in table_filters (LIKE, JSON, AND combinations) -> auto-build
- `LogicalFilter` expressions (OR, complex) -> auto-build
- `ConstantFilter` in table_filters (simple comparisons) -> skip, leave to zone maps

### Alternative: Tracking Wrapper (Zero Overhead, Path 2 Only)

For non-pushable predicates (OR), wrap the LogicalFilter expression with a tracker:

```
Original:
  LogicalFilter(A OR B)
    └── SEQ_SCAN

Modified:
  LogicalFilter(__cache_track(A OR B, rowid))
    └── SEQ_SCAN (+ ROW_ID in column list)
```

`__cache_track(bool result, bigint rowid) -> bool`:
- Returns the original result unchanged
- Records which (rg_idx, vec_idx) had any TRUE results
- On FunctionData destruction (query end) -> stores bitvectors in cache

**Overhead**: Near zero (few bit operations per vector). Only works for Path 2.

### Implementation Roadmap

| Phase | What | Benefit |
|-------|------|---------|
| Phase 1 (done) | Manual `condition_cache_build` + auto-reuse via optimizer | Functional, works |
| Phase 2 | Add row group fingerprint validation | Auto-invalidation on data changes |
| Phase 3 | Inline auto-build in optimizer | No manual build step needed |
| Phase 4 | Expensive predicate detection (only cache LIKE/JSON/OR) | Smart, no wasted cache |
| Phase 5 | LRU eviction + memory budget | Production ready |

## DuckDB Extension Hooks Used

| Hook | Purpose |
|------|---------|
| `OptimizerExtension` | Walk plan tree post-optimization, detect cache hits, inject filter |
| `ExpressionFilter` on `ROW_ID` | Per-vector filter evaluated during scan |
| `CacheExpressionFilter` (subclass) | Override `CheckStatistics()` for row-group pruning |
| `ObjectCache` | Per-database storage for cache entries |
| `TableFunction` | `condition_cache_build`, `condition_cache_clear`, `condition_cache_info` |

No core DuckDB modifications required. All functionality implemented through the extension API.

## Limitations

1. **No automatic population** (current): User must manually call `condition_cache_build()`. Planned for Phase 3.
2. **No auto-invalidation** (current): Cache is not invalidated on data changes. Must manually `condition_cache_clear()`. Planned for Phase 2 via row group fingerprint.
3. **CPU savings only, no I/O savings**: DuckDB's buffer pool caches at row-group granularity. Skipping vectors saves decompression CPU, not disk I/O.
4. **Filter key matching**: Uses canonical serialization of filter expressions. Complex or rewritten predicates may not match.
5. **Best for expensive predicates**: Simple integer/date comparisons see limited benefit because DuckDB's native pushdown already handles them efficiently.
6. **Path 1 zone map concern**: Mixed predicates (simple + expensive) need careful handling to preserve zone map benefits for the simple predicates.
7. **No eviction**: ObjectCache entries are non-evictable. No memory budget enforcement.

## Running Benchmarks

```bash
cd extension-template

# Build
GEN=ninja make reldebug

# Single LIKE benchmark (30M rows)
build/reldebug/duckdb < benchmark_like.sql

# JSON benchmark (30M rows, requires json extension)
build/reldebug/duckdb -c "INSTALL json;"
build/reldebug/duckdb < benchmark_json.sql

# Compound AND benchmark (30M rows)
build/reldebug/duckdb < benchmark_compound.sql

# Compound OR benchmark (30M rows) -- biggest win
build/reldebug/duckdb < benchmark_compound_or.sql

# Simple integer benchmark (50M rows)
build/reldebug/duckdb < benchmark_clickhouse_scenario.sql
```

## Git History

- Branch: `condition-cache-vector-level-prototype`
- V1 commit (vector-level, no speedup): `9673d8f`
- V2 commit (filter movement): `75cafcf`
- V2+ (LIKE/JSON/OR support, row-group pruning, LogicalFilter path): current HEAD
