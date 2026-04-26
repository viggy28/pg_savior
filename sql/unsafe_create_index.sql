LOAD 'pg_savior';
CREATE EXTENSION IF NOT EXISTS pg_savior;

CREATE TABLE emp (id int);
INSERT INTO emp VALUES (1);

-- Plain CREATE INDEX is blocked
CREATE INDEX emp_id_idx ON emp (id);

-- CONCURRENTLY allowed (must be outside a txn block; pg_regress runs each
-- file in its own session and each statement is auto-committed unless
-- explicitly wrapped, so this is fine)
CREATE INDEX CONCURRENTLY emp_id_idx ON emp (id);
DROP INDEX emp_id_idx;

-- Bypass overrides the check
SET pg_savior.bypass = on;
CREATE INDEX emp_id_idx ON emp (id);
DROP INDEX emp_id_idx;
RESET pg_savior.bypass;

-- Disabled: also allowed
SET pg_savior.enabled = off;
CREATE INDEX emp_id_idx ON emp (id);
DROP INDEX emp_id_idx;
SET pg_savior.enabled = on;

DROP TABLE emp;
