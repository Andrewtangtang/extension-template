-- Benchmark V2: Filter movement approach
-- Cache filter is now the ONLY table_filter on the scan.
-- Original predicates moved to LogicalFilter above.
-- This should allow skipping column decompression for non-matching vectors.

.timer off
.print '=== Creating table: 50M rows, spatially concentrated matches ==='
.print '=== Pattern: (i % 60000) < 50  →  ~3.5% of vectors have matches ==='

CREATE TABLE bench AS
SELECT
    i AS id,
    (i % 60000) AS pattern_val,
    md5(i::VARCHAR) AS h1,
    md5((i*2)::VARCHAR) AS h2,
    md5((i*3)::VARCHAR) AS h3,
    md5((i*4)::VARCHAR) AS h4,
    md5((i*5)::VARCHAR) AS h5,
    md5((i*6)::VARCHAR) AS h6,
    repeat('x', 80) AS payload
FROM range(50000000) t(i);
CHECKPOINT;

-- Build cache
SELECT * FROM condition_cache_build('bench', 'pattern_val < 50');
SELECT * FROM condition_cache_info();

-- Warmup both paths
SELECT count(*), sum(length(h1)), sum(length(h2)), sum(length(h3)),
       sum(length(h4)), sum(length(h5)), sum(length(h6)), sum(length(payload))
FROM bench WHERE pattern_val < 50;
SELECT * FROM condition_cache_clear();
SELECT count(*), sum(length(h1)), sum(length(h2)), sum(length(h3)),
       sum(length(h4)), sum(length(h5)), sum(length(h6)), sum(length(payload))
FROM bench WHERE pattern_val < 50;

-- Verify EXPLAIN shows correct plan structure
.print ''
.print '=== EXPLAIN (with cache) ==='
SELECT * FROM condition_cache_build('bench', 'pattern_val < 50');
EXPLAIN SELECT count(*), sum(length(h1)) FROM bench WHERE pattern_val < 50;
SELECT * FROM condition_cache_clear();

.print ''
.print '========================================'
.print '=== WITHOUT CACHE (5 runs) ==='
.print '========================================'
.timer on

SELECT count(*), sum(length(h1)), sum(length(h2)), sum(length(h3)),
       sum(length(h4)), sum(length(h5)), sum(length(h6)), sum(length(payload))
FROM bench WHERE pattern_val < 50;

SELECT count(*), sum(length(h1)), sum(length(h2)), sum(length(h3)),
       sum(length(h4)), sum(length(h5)), sum(length(h6)), sum(length(payload))
FROM bench WHERE pattern_val < 50;

SELECT count(*), sum(length(h1)), sum(length(h2)), sum(length(h3)),
       sum(length(h4)), sum(length(h5)), sum(length(h6)), sum(length(payload))
FROM bench WHERE pattern_val < 50;

SELECT count(*), sum(length(h1)), sum(length(h2)), sum(length(h3)),
       sum(length(h4)), sum(length(h5)), sum(length(h6)), sum(length(payload))
FROM bench WHERE pattern_val < 50;

SELECT count(*), sum(length(h1)), sum(length(h2)), sum(length(h3)),
       sum(length(h4)), sum(length(h5)), sum(length(h6)), sum(length(payload))
FROM bench WHERE pattern_val < 50;

.timer off

-- Build cache
SELECT * FROM condition_cache_build('bench', 'pattern_val < 50');

.print ''
.print '========================================'
.print '=== WITH CACHE (5 runs) ==='
.print '========================================'
.timer on

SELECT count(*), sum(length(h1)), sum(length(h2)), sum(length(h3)),
       sum(length(h4)), sum(length(h5)), sum(length(h6)), sum(length(payload))
FROM bench WHERE pattern_val < 50;

SELECT count(*), sum(length(h1)), sum(length(h2)), sum(length(h3)),
       sum(length(h4)), sum(length(h5)), sum(length(h6)), sum(length(payload))
FROM bench WHERE pattern_val < 50;

SELECT count(*), sum(length(h1)), sum(length(h2)), sum(length(h3)),
       sum(length(h4)), sum(length(h5)), sum(length(h6)), sum(length(payload))
FROM bench WHERE pattern_val < 50;

SELECT count(*), sum(length(h1)), sum(length(h2)), sum(length(h3)),
       sum(length(h4)), sum(length(h5)), sum(length(h6)), sum(length(payload))
FROM bench WHERE pattern_val < 50;

SELECT count(*), sum(length(h1)), sum(length(h2)), sum(length(h3)),
       sum(length(h4)), sum(length(h5)), sum(length(h6)), sum(length(payload))
FROM bench WHERE pattern_val < 50;

.timer off

-- Cleanup
SELECT * FROM condition_cache_clear();
DROP TABLE bench;
