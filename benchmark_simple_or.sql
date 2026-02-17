-- Benchmark: Simple OR predicates (zone-map-friendly predicates combined with OR)
-- Key question: Even simple predicates lose zone map capability when combined with OR
-- Our cache should be the only optimization available

.timer off
.print '=== Simple OR benchmark: 30M rows, simple integer predicates with OR ==='

CREATE TABLE events AS
SELECT
    i AS id,
    (i % 60000) AS val_a,
    (i % 80000) AS val_b,
    md5((i*2)::VARCHAR) AS col1,
    md5((i*3)::VARCHAR) AS col2,
    md5((i*4)::VARCHAR) AS col3,
    md5((i*5)::VARCHAR) AS col4,
    repeat('payload-' || (i%1000)::VARCHAR, 10) AS payload1,
    repeat('x', 100) AS payload2
FROM range(30000000) t(i);
CHECKPOINT;

.print ''
.print '=== EXPLAIN without cache ==='
EXPLAIN SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val_a < 50 OR val_b < 30;

.print ''
.print '=== Cache info ==='
SELECT * FROM condition_cache_build('events', 'val_a < 50 OR val_b < 30');
SELECT * FROM condition_cache_info();
SELECT * FROM condition_cache_clear();

-- Warmup
SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val_a < 50 OR val_b < 30;

.print ''
.print '========================================'
.print '=== WITHOUT CACHE (5 runs) ==='
.print '========================================'
.timer on

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val_a < 50 OR val_b < 30;

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val_a < 50 OR val_b < 30;

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val_a < 50 OR val_b < 30;

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val_a < 50 OR val_b < 30;

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val_a < 50 OR val_b < 30;

.timer off

SELECT * FROM condition_cache_build('events', 'val_a < 50 OR val_b < 30');

.print ''
.print '=== EXPLAIN with cache ==='
EXPLAIN SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val_a < 50 OR val_b < 30;

-- Warmup with cache
SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val_a < 50 OR val_b < 30;

.print ''
.print '========================================'
.print '=== WITH CACHE (5 runs) ==='
.print '========================================'
.timer on

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val_a < 50 OR val_b < 30;

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val_a < 50 OR val_b < 30;

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val_a < 50 OR val_b < 30;

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val_a < 50 OR val_b < 30;

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val_a < 50 OR val_b < 30;

.timer off

SELECT * FROM condition_cache_clear();
DROP TABLE events;
