LOAD 'pg_savior';
CREATE EXTENSION IF NOT EXISTS pg_savior;

-- Lower threshold so a tiny test table counts as "large"
SET pg_savior.large_table_threshold_rows = 10;

-- Small table (under threshold): DROP succeeds
CREATE TABLE small_emp (id int);
INSERT INTO small_emp SELECT generate_series(1, 5);
ANALYZE small_emp;
DROP TABLE small_emp;

-- Large table (over threshold): DROP blocked
CREATE TABLE big_emp (id int);
INSERT INTO big_emp SELECT generate_series(1, 100);
ANALYZE big_emp;
DROP TABLE big_emp;

-- DROP TABLE IF EXISTS on a missing table: allowed (lookup miss, no check fires)
DROP TABLE IF EXISTS does_not_exist;

-- Multi-target DROP: blocked if any target is large
CREATE TABLE small_a (id int);
INSERT INTO small_a VALUES (1);
ANALYZE small_a;
DROP TABLE small_a, big_emp;
-- Both should still exist after the failed multi-drop
SELECT count(*) AS small_a_rows FROM small_a;
SELECT count(*) AS big_emp_rows FROM big_emp;

-- Multi-target DROP where all are small: succeeds
CREATE TABLE small_b (id int);
INSERT INTO small_b VALUES (1);
ANALYZE small_b;
DROP TABLE small_a, small_b;

-- Bypass overrides
SET pg_savior.bypass = on;
DROP TABLE big_emp;
RESET pg_savior.bypass;

-- Disabled: also allowed
CREATE TABLE big_emp (id int);
INSERT INTO big_emp SELECT generate_series(1, 100);
ANALYZE big_emp;
SET pg_savior.enabled = off;
DROP TABLE big_emp;
SET pg_savior.enabled = on;

-- DROP VIEW (not OBJECT_TABLE): not affected by this guard
CREATE TABLE v_src (id int);
INSERT INTO v_src SELECT generate_series(1, 100);
ANALYZE v_src;
CREATE VIEW v_test AS SELECT * FROM v_src;
DROP VIEW v_test;

-- Cleanup
SET pg_savior.bypass = on;
DROP TABLE v_src;
RESET pg_savior.bypass;
