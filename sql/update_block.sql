LOAD 'pg_savior';
CREATE EXTENSION IF NOT EXISTS pg_savior;

CREATE TABLE emp (id int);
INSERT INTO emp VALUES (1), (2), (3);

-- UPDATE without WHERE: must error, table unchanged
UPDATE emp SET id = 99;
SELECT count(*) AS still_one FROM emp WHERE id = 1;
SELECT count(*) AS none_yet  FROM emp WHERE id = 99;

-- UPDATE with WHERE: succeeds
UPDATE emp SET id = 99 WHERE id = 1;
SELECT count(*) AS gone_now  FROM emp WHERE id = 1;
SELECT count(*) AS one_now   FROM emp WHERE id = 99;

DROP TABLE emp;
