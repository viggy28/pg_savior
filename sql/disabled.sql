LOAD 'pg_savior';
CREATE EXTENSION IF NOT EXISTS pg_savior;

CREATE TABLE emp (id int);
INSERT INTO emp VALUES (1), (2), (3);

-- Disabled: allowed
SET pg_savior.enabled = off;
DELETE FROM emp;
SELECT count(*) AS rowcount FROM emp;

-- Re-enabled: blocked
INSERT INTO emp VALUES (1);
SET pg_savior.enabled = on;
DELETE FROM emp;
SELECT count(*) AS rowcount FROM emp;

DROP TABLE emp;
