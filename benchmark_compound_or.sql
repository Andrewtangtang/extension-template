-- Benchmark: Compound LIKE with OR
-- OR can't short-circuit: must evaluate BOTH predicates for every vector
-- Cache should help even more since without-cache is more expensive

.timer off
.print '=== Compound OR benchmark: 30M rows, two LIKE predicates with OR ==='

CREATE TABLE events AS
SELECT
    i AS id,
    CASE WHEN i % 50000 = 0
         THEN 'PREFIX_ERROR_' || md5(i::VARCHAR)
         ELSE md5(i::VARCHAR) || md5((i*7)::VARCHAR)
    END AS text_a,
    CASE WHEN i % 40000 = 0
         THEN 'PREFIX_WARN_' || md5(i::VARCHAR)
         ELSE md5((i*2)::VARCHAR) || md5((i*8)::VARCHAR)
    END AS text_b,
    md5((i*3)::VARCHAR) AS col1,
    md5((i*4)::VARCHAR) AS col2,
    md5((i*5)::VARCHAR) AS col3,
    md5((i*6)::VARCHAR) AS col4,
    repeat('payload-' || (i%1000)::VARCHAR, 10) AS payload1,
    repeat('x', 100) AS payload2
FROM range(30000000) t(i);
CHECKPOINT;

.print ''
.print '=== Cache info: OR ==='
SELECT * FROM condition_cache_build('events', 'text_a LIKE ''%ERROR%'' OR text_b LIKE ''%WARN%''');
SELECT * FROM condition_cache_info();
SELECT * FROM condition_cache_clear();

-- Warmup
SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE text_a LIKE '%ERROR%' OR text_b LIKE '%WARN%';

.print ''
.print '========================================'
.print '=== WITHOUT CACHE (5 runs) ==='
.print '========================================'
.timer on

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE text_a LIKE '%ERROR%' OR text_b LIKE '%WARN%';

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE text_a LIKE '%ERROR%' OR text_b LIKE '%WARN%';

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE text_a LIKE '%ERROR%' OR text_b LIKE '%WARN%';

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE text_a LIKE '%ERROR%' OR text_b LIKE '%WARN%';

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE text_a LIKE '%ERROR%' OR text_b LIKE '%WARN%';

.timer off

SELECT * FROM condition_cache_build('events', 'text_a LIKE ''%ERROR%'' OR text_b LIKE ''%WARN%''');

-- Warmup with cache
SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE text_a LIKE '%ERROR%' OR text_b LIKE '%WARN%';

.print ''
.print '========================================'
.print '=== WITH CACHE (5 runs) ==='
.print '========================================'
.timer on

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE text_a LIKE '%ERROR%' OR text_b LIKE '%WARN%';

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE text_a LIKE '%ERROR%' OR text_b LIKE '%WARN%';

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE text_a LIKE '%ERROR%' OR text_b LIKE '%WARN%';

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE text_a LIKE '%ERROR%' OR text_b LIKE '%WARN%';

SELECT count(*), sum(length(col1)), sum(length(col2)), sum(length(col3)),
       sum(length(col4)), sum(length(payload1)), sum(length(payload2))
FROM events WHERE text_a LIKE '%ERROR%' OR text_b LIKE '%WARN%';

.timer off

SELECT * FROM condition_cache_clear();
DROP TABLE events;
