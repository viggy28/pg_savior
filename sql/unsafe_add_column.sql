LOAD 'pg_savior';
CREATE EXTENSION IF NOT EXISTS pg_savior;

-- Lower the threshold so a tiny test table counts as "large"
SET pg_savior.large_table_threshold_rows = 10;

-- Small table (under threshold): ADD COLUMN with default is allowed
CREATE TABLE small_emp (id int);
INSERT INTO small_emp SELECT generate_series(1, 5);
ANALYZE small_emp;
ALTER TABLE small_emp ADD COLUMN status text DEFAULT 'active';
SELECT status FROM small_emp LIMIT 1;
DROP TABLE small_emp;

-- Large table (over threshold): ADD COLUMN with default is blocked
CREATE TABLE big_emp (id int);
INSERT INTO big_emp SELECT generate_series(1, 100);
ANALYZE big_emp;
ALTER TABLE big_emp ADD COLUMN status text DEFAULT 'active';

-- ADD COLUMN without default: always allowed (no rewrite risk)
ALTER TABLE big_emp ADD COLUMN status text;

-- Same large table: bypass overrides
SET pg_savior.bypass = on;
ALTER TABLE big_emp ADD COLUMN flag text DEFAULT 'set';
RESET pg_savior.bypass;

-- Multiple commands in one ALTER: blocked if any AddColumn has a default
ALTER TABLE big_emp ADD COLUMN x1 int, ADD COLUMN x2 int DEFAULT 0;

-- Multiple commands without any default: allowed
ALTER TABLE big_emp ADD COLUMN y1 int, ADD COLUMN y2 int;

DROP TABLE big_emp;
