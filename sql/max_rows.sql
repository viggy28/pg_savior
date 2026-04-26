LOAD 'pg_savior';
CREATE EXTENSION IF NOT EXISTS pg_savior;

CREATE TABLE emp (id int);
INSERT INTO emp SELECT generate_series(1, 1000);
ANALYZE emp;

-- Default: max_rows_affected = 0, check disabled. WHERE id > 0 estimated ~1000 rows, allowed.
SELECT current_setting('pg_savior.max_rows_affected') AS max_rows;
DELETE FROM emp WHERE id > 999;
SELECT count(*) AS rowcount FROM emp;

-- Threshold = 10. DELETE estimated to touch all remaining rows must be blocked.
SET pg_savior.max_rows_affected = 10;
DELETE FROM emp WHERE id > 0;
SELECT count(*) AS rowcount FROM emp;

-- Under-threshold DELETE succeeds.
DELETE FROM emp WHERE id <= 5;
SELECT count(*) AS rowcount FROM emp;

-- UPDATE over threshold also blocked.
UPDATE emp SET id = id + 10000 WHERE id > 0;
SELECT count(*) AS rowcount FROM emp;

-- Under-threshold UPDATE succeeds.
UPDATE emp SET id = id + 10000 WHERE id <= 10;
SELECT count(*) AS unchanged FROM emp WHERE id <= 10;

-- bypass overrides the row-count check.
SET pg_savior.bypass = on;
DELETE FROM emp WHERE id > 0;
SELECT count(*) AS rowcount FROM emp;
RESET pg_savior.bypass;

-- Refilling so we can re-test the disable path
INSERT INTO emp SELECT generate_series(1, 1000);
ANALYZE emp;

-- Setting threshold back to 0 disables the check; the over-estimate succeeds.
SET pg_savior.max_rows_affected = 0;
DELETE FROM emp WHERE id > 0;
SELECT count(*) AS rowcount FROM emp;

DROP TABLE emp;
