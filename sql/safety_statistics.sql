LOAD 'pg_savior';
CREATE EXTENSION IF NOT EXISTS pg_savior;

SET pg_savior.large_table_threshold_rows = 10;

-- reltuples is -1 until a table is analyzed. The current statistics-based
-- contract treats unknown statistics as not large; this is a documented gap.
CREATE TABLE stats_unknown (id int) WITH (autovacuum_enabled = false);
INSERT INTO stats_unknown SELECT generate_series(1, 100);
SELECT reltuples FROM pg_class WHERE oid = 'stats_unknown'::regclass;
SET pg_savior.max_rows_affected = 10;
DELETE FROM stats_unknown WHERE id > 0;
SELECT count(*) AS unknown_rows_after_estimate_guard FROM stats_unknown;
RESET pg_savior.max_rows_affected;
ALTER TABLE stats_unknown ADD COLUMN marker double precision DEFAULT random();
SELECT count(*) AS unknown_rows_preserved FROM stats_unknown WHERE marker IS NOT NULL;

-- Low statistics can become stale after a bulk insert, producing the same gap.
CREATE TABLE stats_stale_low (id int) WITH (autovacuum_enabled = false);
ANALYZE stats_stale_low;
INSERT INTO stats_stale_low SELECT generate_series(1, 100);
SELECT reltuples FROM pg_class WHERE oid = 'stats_stale_low'::regclass;
SET pg_savior.max_rows_affected = 10;
DELETE FROM stats_stale_low WHERE id > 0;
SELECT count(*) AS stale_low_rows_after_estimate_guard FROM stats_stale_low;
RESET pg_savior.max_rows_affected;
ALTER TABLE stats_stale_low ADD COLUMN marker double precision DEFAULT random();
SELECT count(*) AS stale_low_rows_preserved FROM stats_stale_low WHERE marker IS NOT NULL;

-- High stale statistics can conservatively block an operation on an empty table.
CREATE TABLE stats_stale_high (id int) WITH (autovacuum_enabled = false);
INSERT INTO stats_stale_high SELECT generate_series(1, 100);
ANALYZE stats_stale_high;
SET pg_savior.bypass = on;
DELETE FROM stats_stale_high WHERE true;
RESET pg_savior.bypass;
SELECT reltuples, (SELECT count(*) FROM stats_stale_high) AS actual_rows
FROM pg_class WHERE oid = 'stats_stale_high'::regclass;
SET pg_savior.max_rows_affected = 10;
DELETE FROM stats_stale_high WHERE id > 0;
RESET pg_savior.max_rows_affected;
ALTER TABLE stats_stale_high ALTER COLUMN id TYPE bigint;
SELECT atttypid = 'integer'::regtype AS stale_high_type_unchanged
FROM pg_attribute
WHERE attrelid = 'stats_stale_high'::regclass AND attname = 'id';

SET pg_savior.bypass = on;
DROP TABLE stats_unknown;
DROP TABLE stats_stale_low;
DROP TABLE stats_stale_high;
RESET pg_savior.bypass;
