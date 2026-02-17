# Query Condition Cache for DuckDB — Design V2

## Background: Lessons from Prototype

We built a prototype condition cache extension for DuckDB (v1.4.4) and benchmarked it extensively. Key findings:

1. **DuckDB's native filter pushdown already skips output column decompression** for non-matching vectors. For pushed-down predicates (simple comparisons), the cache only additionally saves filter column decompression — negligible for integers.

2. **DuckDB's buffer pool caches at row-group level (122,880 rows)**. Unlike ClickHouse (8,192-row granules), skipping vectors within a row group doesn't save I/O — the blocks are already loaded.

3. **The real opportunity: predicates that DuckDB CANNOT push down** (e.g., `LIKE '%pattern%'`, complex expressions). For these, DuckDB decompresses ALL columns for ALL vectors, then filters in a LogicalFilter above the scan. A condition cache can skip 95%+ of this decompression.

## Target Use Case

Dashboard/alert queries with expensive, non-indexable predicates:

```sql
-- LIKE can't be pushed down as table_filter → full column decompression
SELECT user_id, event_time, payload
FROM events
WHERE event_text LIKE '%error_code_42%'
  AND region = 'us-east-1'
  AND ts > '2025-01-01';
```

- `region = 'us-east-1'` and `ts > '2025-01-01'`: simple comparisons → zone maps + filter pushdown handle these natively
- `event_text LIKE '%error_code_42%'`: expensive, not pushable → **this is where the cache adds value**

## Cache Structure

### Key

```
TableID : RowGroupID : PredicateHash
```

- **TableID**: identifies the table (catalog OID or name)
- **RowGroupID**: identifies the row group within the table
- **PredicateHash**: hash of the normalized predicate string (e.g., `hash("event_text LIKE '%error_code_42%'")`)

### Value

```
uint64_t vector_mask   (60 bits used, one per vector in the row group)
```

- Bit `i` = 1: vector `i` has at least one row matching the predicate
- Bit `i` = 0: vector `i` has NO matching rows → safe to skip entirely
- 122,880 rows / 2,048 rows per vector = 60 vectors per row group → fits in one `uint64_t`

### Example

```
Table: events (TableID = 5)
Predicate: event_text LIKE '%error_code_42%' (PredicateHash = 0xABCD1234)

Key: 5:0:0xABCD1234  → Value: 0b0000...0100100001  (vectors 0, 5, 8 have matches)
Key: 5:1:0xABCD1234  → Value: 0b0000...0000000000  (no matches → skip entire row group)
Key: 5:2:0xABCD1234  → Value: 0b0000...0010000100  (vectors 2, 7 have matches)
...
```

## Execution Flow

```
Query: SELECT ... FROM events
       WHERE event_text LIKE '%error_code_42%'    ← expensive, not pushable
         AND region = 'us-east-1'                  ← simple, pushable
         AND ts > '2025-01-01'                     ← simple, pushable

Execution plan:

┌───────────────────────────┐
│    LogicalFilter          │
│  event_text LIKE '...'    │  ← only evaluates on surviving rows
└─────────────┬─────────────┘
┌─────────────┴─────────────┐
│    SEQ_SCAN (events)      │
│                           │
│  Zone maps:               │  ← Step 1: skip row groups where
│    region = 'us-east-1'   │     min/max stats guarantee no match
│    ts > '2025-01-01'      │
│                           │
│  Cache filter:            │  ← Step 2: within surviving row groups,
│    condition_cache(rowid) │     skip vectors with no LIKE matches
└───────────────────────────┘
```

### Step-by-step for each row group:

```
1. Zone map check (region, ts)
   → FILTER_ALWAYS_FALSE? → skip entire row group (no I/O if not in buffer pool)

2. For each vector in the row group:
   a. Cache filter: lookup vector_mask[vector_idx]
      → bit = 0? → Skip() ALL columns → next vector
      → bit = 1? → continue to step b

   b. Decompress pushed-down filter columns (region, ts)
      → Evaluate region = 'us-east-1' AND ts > '2025-01-01'
      → No rows pass? → Skip() remaining columns → next vector

   c. Decompress output columns (event_text, user_id, event_time, payload)
      → Output to LogicalFilter above

3. LogicalFilter evaluates: event_text LIKE '%error_code_42%'
   → Only runs on rows that passed steps 1-2
```

## Bit Operation Composition

Multiple cached predicates can be combined with bitwise operations:

```
Cache A: LIKE '%error%'       → vector_mask_A per row group
Cache B: LIKE '%timeout%'     → vector_mask_B per row group

Query: WHERE ... LIKE '%error%' AND ... LIKE '%timeout%'
  → vector_mask = mask_A & mask_B    (AND → intersection)

Query: WHERE ... LIKE '%error%' OR ... LIKE '%timeout%'
  → vector_mask = mask_A | mask_B    (OR → union)
```

Since each mask is a `uint64_t`, AND/OR is a single CPU instruction. This enables:
- Combining multiple expensive predicates cheaply
- Reusing individual predicate caches across different query shapes
- Incremental cache building (cache each predicate independently)

## Zone Map Interaction

DuckDB stores min/max statistics **per column segment** (= per row group per column). Zone maps operate at row-group level.

```
                     Zone Maps              Condition Cache
Granularity:         Row group (122,880)    Vector (2,048)
Predicate types:     Simple (=, <, >, IN)   Any (LIKE, complex expressions)
Storage:             Built-in (always)      Extension (explicit build)
I/O savings:         Yes (skip row group)   Yes (if row group not in buffer pool)
CPU savings:         Yes (skip row group)   Yes (skip vector decompression)
```

They're complementary:
- Zone maps handle simple predicates that align with data distribution
- Condition cache handles expensive predicates that zone maps can't help with
- **Zone maps run first** (row-group level), then condition cache (vector level within surviving row groups)

No special integration needed — zone maps already run during `CheckZonemap()` before the per-vector scan loop. The condition cache filter runs inside the vector loop.

## What Changes from Prototype

| | Prototype (current) | V2 Design |
|---|---|---|
| Target predicates | Pushed-down table_filters only | Non-pushable predicates (LIKE, etc.) |
| Optimizer matching | Match `get.table_filters` | Match LogicalFilter expressions above scan |
| Cache key | `table_name\|filter_string` | `TableID:RowGroupID:PredicateHash` |
| Composition | Single predicate per entry | Bitwise AND/OR of multiple predicates |
| Build trigger | Manual `condition_cache_build()` | Manual (future: auto on first execution) |
| Invalidation | Manual `condition_cache_clear()` | Manual (future: hook into WAL/commit) |

### Key Implementation Change: Match LogicalFilter Instead of TableFilters

Current prototype only matches predicates in `get.table_filters`. But the most valuable predicates (LIKE) are NOT in table_filters — they stay in `LogicalFilter` above the scan.

V2 needs to:
1. Walk the plan tree, find `LogicalFilter` → `LogicalGet` patterns
2. For each LogicalFilter expression, compute PredicateHash
3. Look up cache by (TableID, PredicateHash)
4. If hit: inject cache filter on the scan, keep LogicalFilter above for correctness
5. No need to move original filters (they're already above the scan)

This is actually **simpler** than the prototype's filter movement approach — we don't need to move anything, just inject the cache filter alongside the existing scan.

## Open Questions

1. **Cache population for LIKE predicates**: `condition_cache_build('events', "event_text LIKE '%pattern%'")` runs a full scan internally. For very large tables, this initial cost may be high. Could amortize by building during the first query execution.

2. **Predicate normalization**: How to normalize predicates for consistent hashing? `LIKE '%x%'` and `like '%x%'` should hash the same. Need canonical form.

3. **Cache invalidation**: After INSERT/UPDATE/DELETE, cached masks may be stale. Options:
   - Manual clear (current)
   - Hook into transaction commit to invalidate affected row groups
   - Versioned entries (check row group version on lookup)

4. **Memory management**: With many predicates × many row groups, cache size could grow. Need eviction policy (LRU by predicate, or limit total entries).
