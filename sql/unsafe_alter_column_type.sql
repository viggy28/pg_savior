LOAD 'pg_savior';
CREATE EXTENSION IF NOT EXISTS pg_savior;

SET pg_savior.large_table_threshold_rows = 10;

-- Small table: ALTER COLUMN TYPE allowed
CREATE TABLE small_emp (id int);
INSERT INTO small_emp SELECT generate_series(1, 5);
ANALYZE small_emp;
ALTER TABLE small_emp ALTER COLUMN id TYPE bigint;
DROP TABLE small_emp;

-- Large table: ALTER COLUMN TYPE blocked
CREATE TABLE big_emp (id int, name text);
INSERT INTO big_emp SELECT generate_series(1, 100), 'x';
ANALYZE big_emp;
ALTER TABLE big_emp ALTER COLUMN id TYPE bigint;

-- Multi-cmd: blocked if any cmd is a type change
ALTER TABLE big_emp ADD COLUMN extra int, ALTER COLUMN name TYPE varchar(50);

-- Multi-cmd without type change or default: allowed
ALTER TABLE big_emp ADD COLUMN y1 int, ADD COLUMN y2 int;

-- Bypass overrides
SET pg_savior.bypass = on;
ALTER TABLE big_emp ALTER COLUMN id TYPE bigint;
RESET pg_savior.bypass;

-- Disabled also allowed
SET pg_savior.enabled = off;
ALTER TABLE big_emp ALTER COLUMN name TYPE varchar(100);
SET pg_savior.enabled = on;

-- Cleanup
SET pg_savior.bypass = on;
DROP TABLE big_emp;
RESET pg_savior.bypass;
