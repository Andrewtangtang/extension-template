-- Benchmark: Mixed AND predicate (simple + expensive)
-- Tests that zone map is preserved for simple predicates while cache handles expensive ones
-- WHERE col_int < 50 AND text LIKE '%ERROR%'

.timer off
.print '=== Mixed AND benchmark: 30M rows, integer + LIKE ==='

CREATE TABLE events AS
SELECT
    i AS id,
    (i % 60000) AS val,
    CASE WHEN i % 50000 = 0
         THEN 'PREFIX_ERROR_' || md5(i::VARCHAR)
         ELSE md5(i::VARCHAR) || md5((i*7)::VARCHAR)
    END AS text_col,
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
FROM events WHERE val < 50 AND text_col LIKE '%ERROR%';

.print ''
.print '=== Cache info ==='
SELECT * FROM condition_cache_build('events', 'val < 50 AND text_col LIKE ''%ERROR%''');
SELECT * FROM condition_cache_info();

.print ''
.print '=== EXPLAIN with cache (should show val<50 STILL on scan + cache filter) ==='
EXPLAIN SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val < 50 AND text_col LIKE '%ERROR%';

SELECT * FROM condition_cache_clear();

-- Warmup
SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val < 50 AND text_col LIKE '%ERROR%';

.print ''
.print '========================================'
.print '=== WITHOUT CACHE (5 runs) ==='
.print '========================================'
.timer on

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val < 50 AND text_col LIKE '%ERROR%';

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val < 50 AND text_col LIKE '%ERROR%';

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val < 50 AND text_col LIKE '%ERROR%';

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val < 50 AND text_col LIKE '%ERROR%';

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val < 50 AND text_col LIKE '%ERROR%';

.timer off

SELECT * FROM condition_cache_build('events', 'val < 50 AND text_col LIKE ''%ERROR%''');

-- Warmup with cache
SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val < 50 AND text_col LIKE '%ERROR%';

.print ''
.print '========================================'
.print '=== WITH CACHE (5 runs) ==='
.print '========================================'
.timer on

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val < 50 AND text_col LIKE '%ERROR%';

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val < 50 AND text_col LIKE '%ERROR%';

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val < 50 AND text_col LIKE '%ERROR%';

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val < 50 AND text_col LIKE '%ERROR%';

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE val < 50 AND text_col LIKE '%ERROR%';

.timer off

SELECT * FROM condition_cache_clear();
DROP TABLE events;
