LOAD 'pg_savior';
CREATE EXTENSION IF NOT EXISTS pg_savior;

CREATE TABLE emp (id int);
CREATE TABLE dept (id int);
INSERT INTO emp VALUES (1), (2), (3);
INSERT INTO dept VALUES (1);

-- DELETE without WHERE: must error, table unchanged
DELETE FROM emp;
SELECT count(*) AS rowcount FROM emp;

-- DELETE with WHERE: succeeds
DELETE FROM emp WHERE id = 1;
SELECT count(*) AS rowcount FROM emp;

-- DELETE with subquery WHERE: succeeds (no HashJoin special case needed)
DELETE FROM emp WHERE id IN (SELECT id FROM dept);
SELECT count(*) AS rowcount FROM emp;

DROP TABLE emp;
DROP TABLE dept;
