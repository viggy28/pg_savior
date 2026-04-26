LOAD 'pg_savior';
CREATE EXTENSION IF NOT EXISTS pg_savior;

SET pg_savior.large_table_threshold_rows = 10;

-- Small table: TRUNCATE allowed
CREATE TABLE small_emp (id int);
INSERT INTO small_emp SELECT generate_series(1, 5);
ANALYZE small_emp;
TRUNCATE small_emp;
SELECT count(*) AS rowcount FROM small_emp;

-- Large table: TRUNCATE blocked
CREATE TABLE big_emp (id int);
INSERT INTO big_emp SELECT generate_series(1, 100);
ANALYZE big_emp;
TRUNCATE big_emp;
SELECT count(*) AS rowcount FROM big_emp;

-- Multi-target: blocked if any large
INSERT INTO small_emp SELECT generate_series(1, 5);
ANALYZE small_emp;
TRUNCATE small_emp, big_emp;
-- Both still populated
SELECT count(*) AS small_rowcount FROM small_emp;
SELECT count(*) AS big_rowcount FROM big_emp;

-- Multi-target where all are small: succeeds
CREATE TABLE small_b (id int);
INSERT INTO small_b VALUES (1);
ANALYZE small_b;
TRUNCATE small_emp, small_b;

-- TRUNCATE ... CASCADE: same logic (CASCADE doesn't affect our check)
TRUNCATE big_emp CASCADE;

-- Bypass overrides
SET pg_savior.bypass = on;
TRUNCATE big_emp;
RESET pg_savior.bypass;
SELECT count(*) AS after_bypass FROM big_emp;

-- Disabled: also allowed
INSERT INTO big_emp SELECT generate_series(1, 100);
ANALYZE big_emp;
SET pg_savior.enabled = off;
TRUNCATE big_emp;
SET pg_savior.enabled = on;
SELECT count(*) AS after_disabled FROM big_emp;

-- Cleanup
SET pg_savior.bypass = on;
DROP TABLE big_emp;
DROP TABLE small_emp;
DROP TABLE small_b;
RESET pg_savior.bypass;
