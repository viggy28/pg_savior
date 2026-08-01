LOAD 'pg_savior';
CREATE EXTENSION IF NOT EXISTS pg_savior;

CREATE TABLE matrix_emp (id int PRIMARY KEY, value int);
CREATE TABLE matrix_ids (id int PRIMARY KEY);
INSERT INTO matrix_emp VALUES (1, 10), (2, 20), (3, 30), (4, 40);
INSERT INTO matrix_ids VALUES (1), (3);
ANALYZE matrix_emp;
ANALYZE matrix_ids;

-- Data-modifying CTEs are still DELETE/UPDATE statements and must be checked.
WITH removed AS (
  DELETE FROM matrix_emp RETURNING id
)
SELECT count(*) FROM removed;
SELECT count(*) AS rows_after_cte_delete FROM matrix_emp;

WITH changed AS (
  UPDATE matrix_emp SET value = 0 RETURNING id
)
SELECT count(*) FROM changed;
SELECT count(*) AS unchanged_after_cte_update FROM matrix_emp WHERE value > 0;

-- A read-only CTE and subquery predicate are real WHERE clauses and are allowed.
WITH selected AS (SELECT id FROM matrix_ids)
DELETE FROM matrix_emp WHERE id IN (SELECT id FROM selected);
SELECT array_agg(id ORDER BY id) AS after_cte_predicate FROM matrix_emp;

-- UPDATE with a subquery predicate is also allowed.
UPDATE matrix_emp SET value = value + 1
WHERE id = 2 AND EXISTS (SELECT 1 FROM matrix_ids WHERE matrix_ids.id = 1);
SELECT value FROM matrix_emp WHERE id = 2;

-- RETURNING does not count as a WHERE clause.
DELETE FROM matrix_emp RETURNING id;
SELECT count(*) AS rows_after_returning_block FROM matrix_emp;

-- PREPARE analyzes the inner statement, so an unsafe prepared query is rejected.
PREPARE unsafe_delete AS DELETE FROM matrix_emp;
SELECT count(*) AS unsafe_prepared_count
FROM pg_prepared_statements WHERE name = 'unsafe_delete';

-- Parameterized predicates remain allowed and affect only the selected row.
PREPARE safe_delete(int) AS DELETE FROM matrix_emp WHERE id = $1 RETURNING id;
EXECUTE safe_delete(2);
DEALLOCATE safe_delete;
SELECT array_agg(id ORDER BY id) AS after_prepared_delete FROM matrix_emp;

-- Literal tautologies are intentionally treated as explicit WHERE clauses.
-- Use max_rows_affected when semantic affected-row protection is required.
TRUNCATE matrix_emp;
INSERT INTO matrix_emp SELECT id, id FROM generate_series(1, 100) AS id;
UPDATE matrix_emp SET value = 99 WHERE true;
SELECT count(*) AS tautology_updated FROM matrix_emp WHERE value = 99;

-- The estimate guard must also inspect a data-modifying CTE under an outer SELECT.
TRUNCATE matrix_emp;
INSERT INTO matrix_emp SELECT id, id FROM generate_series(1, 100) AS id;
ANALYZE matrix_emp;
SET pg_savior.max_rows_affected = 10;
WITH removed AS (
  DELETE FROM matrix_emp WHERE id > 0 RETURNING id
)
SELECT count(*) FROM removed;
SELECT count(*) AS rows_after_cte_estimate_block FROM matrix_emp;
RESET pg_savior.max_rows_affected;

DROP TABLE matrix_emp;
DROP TABLE matrix_ids;
