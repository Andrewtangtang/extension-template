# Condition Cache Benchmarks

30M rows, 9 columns (including payload columns), after CHECKPOINT. 5 runs each, median reported.

## Results

| Benchmark | Predicate | Without Cache | With Cache | Speedup |
|-----------|-----------|---------------|------------|---------|
| Compound OR | `text_a LIKE '%ERROR%' OR text_b LIKE '%WARN%'` | 2.59s | 0.15s | **~17x** |
| Simple OR | `val_a < 50 OR val_b < 30` | 1.88s | 0.013s | **~145x** |
| Mixed AND | `val < 50 AND text_col LIKE '%ERROR%'` | 0.023s | 0.005s | **~4x** |

## Cache Selectivity

| Benchmark | Matching Vectors | Total Vectors | Skip Rate |
|-----------|-----------------|---------------|-----------|
| Compound OR | 1,200 | 14,640 | 91.8% |
| Simple OR | 758 | 14,640 | 94.8% |
| Mixed AND | 100 | 6,000 | 98.3% |

## Analysis

- **Simple OR** benefits most: zone maps cannot help with OR across columns, and the predicates themselves are cheap — the bottleneck is decompression of all 9 columns for every vector. Cache eliminates 94.8% of decompression work.
- **Compound OR** shows strong gains: two LIKE evaluations per vector are expensive. Cache skips both LIKE and decompression for 91.8% of vectors.
- **Mixed AND** shows modest gains: zone map already prunes `val < 50` at row-group level, leaving fewer vectors for the cache to skip. The cache still helps by eliminating LIKE evaluation on surviving vectors where no rows match.

## Raw Data

### Compound OR

```
WITHOUT CACHE (5 runs): 2.656s, 2.583s, 2.589s, 2.591s, 2.587s
WITH CACHE    (5 runs): 0.154s, 0.150s, 0.149s, 0.147s, 0.148s
```

### Simple OR

```
WITHOUT CACHE (5 runs): 1.899s, 1.881s, 1.883s, 1.883s, 1.885s
WITH CACHE    (5 runs): 0.014s, 0.013s, 0.013s, 0.013s, 0.013s
```

### Mixed AND

```
WITHOUT CACHE (5 runs): 0.027s, 0.023s, 0.023s, 0.022s, 0.023s
WITH CACHE    (5 runs): 0.006s, 0.005s, 0.005s, 0.005s, 0.005s
```
