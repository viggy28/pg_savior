LOAD 'pg_savior';
CREATE EXTENSION IF NOT EXISTS pg_savior;

CREATE TABLE emp (id int);
INSERT INTO emp VALUES (1), (2), (3);

-- Default: blocked
DELETE FROM emp;
SELECT count(*) AS rowcount FROM emp;

-- Bypass on: allowed
SET pg_savior.bypass = on;
DELETE FROM emp;
SELECT count(*) AS rowcount FROM emp;

-- Reset: blocked again
INSERT INTO emp VALUES (1);
RESET pg_savior.bypass;
DELETE FROM emp;
SELECT count(*) AS rowcount FROM emp;

DROP TABLE emp;
