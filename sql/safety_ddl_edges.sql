LOAD 'pg_savior';
CREATE EXTENSION IF NOT EXISTS pg_savior;

SET pg_savior.large_table_threshold_rows = 10;

-- IF NOT EXISTS is allowed only when the named relation already makes it a no-op.
CREATE TABLE index_target (id int);
CREATE INDEX CONCURRENTLY index_target_id_idx ON index_target (id);
CREATE INDEX IF NOT EXISTS index_target_id_idx ON index_target (id);
CREATE INDEX IF NOT EXISTS index_target_missing_idx ON index_target (id);
SELECT to_regclass('index_target_missing_idx') IS NULL AS missing_index_was_blocked;
DROP INDEX index_target_id_idx;
DROP TABLE index_target;

-- Index-backed constraints on an existing large table build an index while locked.
CREATE TABLE big_constraint (id int);
INSERT INTO big_constraint SELECT generate_series(1, 100);
ANALYZE big_constraint;
ALTER TABLE big_constraint ADD CONSTRAINT big_constraint_unsafe UNIQUE (id);
SELECT count(*) AS unsafe_constraint_count
FROM pg_constraint WHERE conname = 'big_constraint_unsafe';

-- Column-level UNIQUE/PRIMARY KEY constraints also build indexes.
ALTER TABLE big_constraint ADD COLUMN unsafe_column int UNIQUE;
SELECT count(*) AS unsafe_column_count
FROM information_schema.columns
WHERE table_name = 'big_constraint' AND column_name = 'unsafe_column';
ALTER TABLE big_constraint ADD COLUMN unsafe_primary int PRIMARY KEY;
SELECT count(*) AS unsafe_primary_count
FROM information_schema.columns
WHERE table_name = 'big_constraint' AND column_name = 'unsafe_primary';

-- Validated CHECK/FK constraints scan existing rows; NOT VALID is the safe form.
ALTER TABLE big_constraint ADD CONSTRAINT big_constraint_validated CHECK (id > 0);
SELECT count(*) AS validated_constraint_count
FROM pg_constraint WHERE conname = 'big_constraint_validated';
ALTER TABLE big_constraint
  ADD CONSTRAINT big_constraint_not_valid CHECK (id > 0) NOT VALID;
SELECT count(*) AS not_valid_constraint_count
FROM pg_constraint WHERE conname = 'big_constraint_not_valid';

-- Safe migration path: build concurrently, then attach the existing index.
CREATE UNIQUE INDEX CONCURRENTLY big_constraint_id_idx ON big_constraint (id);
ALTER TABLE big_constraint
  ADD CONSTRAINT big_constraint_safe UNIQUE USING INDEX big_constraint_id_idx;
SELECT count(*) AS safe_constraint_count
FROM pg_constraint WHERE conname = 'big_constraint_safe';

-- PRIMARY KEY USING INDEX may scan to enforce NOT NULL, so it stays guarded.
ALTER TABLE big_constraint ADD COLUMN nullable_id int;
UPDATE big_constraint SET nullable_id = id WHERE id > 1;
CREATE UNIQUE INDEX CONCURRENTLY big_constraint_nullable_idx
  ON big_constraint (nullable_id);
ALTER TABLE big_constraint
  ADD CONSTRAINT big_constraint_pk PRIMARY KEY
  USING INDEX big_constraint_nullable_idx;
SELECT count(*) AS using_index_pk_count
FROM pg_constraint WHERE conname = 'big_constraint_pk';

-- The same generated constraint index is allowed on a small existing table.
CREATE TABLE small_constraint (id int);
INSERT INTO small_constraint VALUES (1), (2);
ANALYZE small_constraint;
ALTER TABLE small_constraint ADD CONSTRAINT small_constraint_u UNIQUE (id);
ALTER TABLE small_constraint ADD COLUMN other_id int UNIQUE;

-- Generated indexes on new ordinary and partitioned tables remain allowed.
CREATE TABLE generated_keys (id int PRIMARY KEY, email text UNIQUE);
CREATE TABLE generated_partitioned (id int PRIMARY KEY) PARTITION BY RANGE (id);
CREATE TABLE generated_partitioned_1 PARTITION OF generated_partitioned
  FOR VALUES FROM (0) TO (100);

-- Threshold equality is allowed; one row above is blocked.
CREATE TABLE exactly_threshold (id int);
INSERT INTO exactly_threshold SELECT generate_series(1, 10);
ANALYZE exactly_threshold;
TRUNCATE exactly_threshold;
SELECT count(*) AS exact_rows_after_truncate FROM exactly_threshold;

CREATE TABLE above_threshold (id int);
INSERT INTO above_threshold SELECT generate_series(1, 11);
ANALYZE above_threshold;
TRUNCATE above_threshold;
SELECT count(*) AS above_rows_after_truncate FROM above_threshold;

-- Multi-target order and missing IF EXISTS targets cannot hide a large table.
CREATE TABLE tiny_target (id int);
INSERT INTO tiny_target VALUES (1);
ANALYZE tiny_target;
TRUNCATE above_threshold, tiny_target;
SELECT count(*) AS above_after_multi_truncate FROM above_threshold;
SELECT count(*) AS tiny_after_multi_truncate FROM tiny_target;

DROP TABLE IF EXISTS absent_target, above_threshold;
SELECT to_regclass('above_threshold') IS NOT NULL AS above_survived_multi_drop;

SET pg_savior.bypass = on;
DROP TABLE above_threshold;
DROP TABLE big_constraint;
RESET pg_savior.bypass;
DROP TABLE tiny_target;
DROP TABLE exactly_threshold;
DROP TABLE small_constraint;
DROP TABLE generated_partitioned;
DROP TABLE generated_keys;
