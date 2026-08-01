LOAD 'pg_savior';
CREATE EXTENSION IF NOT EXISTS pg_savior;

SET pg_savior.large_table_threshold_rows = 10;
CREATE DOMAIN alter_free_domain AS integer;
CREATE DOMAIN alter_checked_domain AS integer CHECK (VALUE > 0);

-- Core's no-heap-rewrite shapes are allowed, including an explicit identity USING.
CREATE TABLE alter_allowed (
    widening varchar(10), binary_compatible text, identity_using integer,
    free_domain integer, ts timestamp, tstz timestamptz, indexed varchar(10),
    defaulted varchar(10) DEFAULT 'abc'
);
INSERT INTO alter_allowed
SELECT 'abc', 'abc', g, g, timestamp '2020-01-01', timestamptz '2020-01-01 UTC',
       'abc', 'abc'
FROM generate_series(1, 100) AS g;
ANALYZE alter_allowed;
SET pg_savior.bypass = on;
CREATE INDEX alter_allowed_idx ON alter_allowed (indexed);
RESET pg_savior.bypass;
SELECT pg_relation_filenode('alter_allowed') AS allowed_heap_before,
       pg_relation_filenode('alter_allowed_idx') AS allowed_index_before \gset

ALTER TABLE alter_allowed ALTER widening TYPE varchar(40);
ALTER TABLE alter_allowed ALTER binary_compatible TYPE varchar;
ALTER TABLE alter_allowed ALTER identity_using TYPE integer USING identity_using;
ALTER TABLE alter_allowed ALTER free_domain TYPE alter_free_domain;
SET TIME ZONE 'UTC';
ALTER TABLE alter_allowed ALTER ts TYPE timestamptz;
ALTER TABLE alter_allowed ALTER tstz TYPE timestamp;
ALTER TABLE alter_allowed ALTER indexed TYPE varchar(40);
ALTER TABLE alter_allowed ALTER defaulted TYPE varchar(40);
SELECT :'allowed_heap_before'::oid = pg_relation_filenode('alter_allowed') AS allowed_heap_unchanged,
       pg_typeof(widening) = 'character varying'::regtype AS widening_type,
       pg_typeof(binary_compatible) = 'character varying'::regtype AS binary_type,
       pg_typeof(free_domain) = 'alter_free_domain'::regtype AS domain_type,
       (SELECT column_default IS NOT NULL
        FROM information_schema.columns
        WHERE table_name = 'alter_allowed' AND column_name = 'defaulted') AS default_preserved
FROM alter_allowed LIMIT 1;
SELECT :'allowed_index_before'::oid = pg_relation_filenode('alter_allowed_idx') AS index_filenode_reused;

-- A heap-stable collation change may still rebuild a dependent index.
CREATE TABLE alter_collation (value text COLLATE "C");
INSERT INTO alter_collation SELECT g::text FROM generate_series(1, 100) AS g;
ANALYZE alter_collation;
SET pg_savior.bypass = on;
CREATE INDEX alter_collation_idx ON alter_collation (value);
RESET pg_savior.bypass;
SELECT pg_relation_filenode('alter_collation') AS collation_heap_before,
       pg_relation_filenode('alter_collation_idx') AS collation_index_before \gset
ALTER TABLE alter_collation ALTER value TYPE text COLLATE "POSIX";
SELECT :'collation_heap_before'::oid = pg_relation_filenode('alter_collation') AS collation_heap_unchanged,
       :'collation_index_before'::oid <> pg_relation_filenode('alter_collation_idx') AS collation_index_rebuilt;

-- Every other planned shape is treated as a heap rewrite.
CREATE TABLE alter_blocked (
    as_bigint integer, narrowing varchar(20), arbitrary integer,
    other_column integer, wrong_var integer,
    checked_domain integer, non_utc timestamp
);
INSERT INTO alter_blocked
SELECT g, 'abc', g, g + 1, g, g, timestamp '2020-01-01'
FROM generate_series(1, 100) AS g;
ANALYZE alter_blocked;
SELECT pg_relation_filenode('alter_blocked') AS blocked_heap_before \gset
ALTER TABLE alter_blocked ALTER as_bigint TYPE bigint;
ALTER TABLE alter_blocked ALTER narrowing TYPE varchar(5);
ALTER TABLE alter_blocked ALTER arbitrary TYPE integer USING arbitrary + 0;
ALTER TABLE alter_blocked ALTER wrong_var TYPE integer USING other_column;
ALTER TABLE alter_blocked ALTER checked_domain TYPE alter_checked_domain;
SET TIME ZONE 'America/New_York';
ALTER TABLE alter_blocked ALTER non_utc TYPE timestamptz;
SELECT :'blocked_heap_before'::oid = pg_relation_filenode('alter_blocked') AS blocked_heap_unchanged,
       pg_typeof(as_bigint) = 'integer'::regtype AS bigint_blocked,
       pg_typeof(narrowing) = 'character varying'::regtype AS narrowing_blocked,
       pg_typeof(checked_domain) = 'integer'::regtype AS domain_blocked
FROM alter_blocked LIMIT 1;

-- Invalid coercions and non-rebuildable dependencies retain core errors.
CREATE TABLE alter_invalid (value text);
INSERT INTO alter_invalid SELECT '1' FROM generate_series(1, 100);
ANALYZE alter_invalid;
ALTER TABLE alter_invalid ALTER value TYPE integer;
CREATE VIEW alter_invalid_view AS SELECT value FROM alter_invalid;
ALTER TABLE alter_invalid ALTER value TYPE varchar;
ALTER TABLE alter_invalid ALTER value TYPE bigint USING value::bigint;

CREATE TABLE alter_bad_default (value integer DEFAULT 1);
INSERT INTO alter_bad_default SELECT g FROM generate_series(1, 100) AS g;
ANALYZE alter_bad_default;
ALTER TABLE alter_bad_default ALTER value TYPE timestamp USING to_timestamp(value);

-- Generated/identity properties retain core errors or are blocked before rewrite.
CREATE TABLE alter_generated (
    source text,
    generated_value text GENERATED ALWAYS AS (source) STORED
);
INSERT INTO alter_generated SELECT g::text FROM generate_series(1, 100) AS g;
ANALYZE alter_generated;
ALTER TABLE alter_generated ALTER generated_value TYPE integer USING generated_value::integer;

CREATE TABLE alter_identity (id integer GENERATED ALWAYS AS IDENTITY, filler text);
INSERT INTO alter_identity (filler) SELECT 'x' FROM generate_series(1, 100);
ANALYZE alter_identity;
ALTER TABLE alter_identity ALTER id TYPE bigint;
ALTER TABLE alter_identity ALTER id TYPE text USING id::text;
SELECT pg_typeof(id) = 'integer'::regtype AS identity_type_unchanged
FROM alter_identity LIMIT 1;

-- A small inheritance parent cannot hide a large physical child.
CREATE TABLE alter_inh_parent (id integer, label text);
CREATE TABLE alter_inh_child () INHERITS (alter_inh_parent);
INSERT INTO alter_inh_parent VALUES (1, 'parent');
INSERT INTO alter_inh_child SELECT g, 'child' FROM generate_series(1, 100) AS g;
ANALYZE alter_inh_parent;
ANALYZE alter_inh_child;
SELECT pg_relation_filenode('alter_inh_parent') AS inh_parent_before,
       pg_relation_filenode('alter_inh_child') AS inh_child_before \gset
ALTER TABLE alter_inh_parent ALTER id TYPE bigint;
ALTER TABLE ONLY alter_inh_parent ALTER id TYPE bigint;
ALTER TABLE alter_inh_parent ALTER label TYPE varchar;
SELECT :'inh_parent_before'::oid = pg_relation_filenode('alter_inh_parent') AS inh_parent_unchanged,
       :'inh_child_before'::oid = pg_relation_filenode('alter_inh_child') AS inh_child_unchanged,
       pg_typeof(label) = 'character varying'::regtype AS inherited_binary_type
FROM ONLY alter_inh_parent;

-- Core errors on later descendants still take precedence after a large child.
CREATE TABLE alter_dep_parent (id integer);
CREATE TABLE alter_dep_large () INHERITS (alter_dep_parent);
CREATE TABLE alter_dep_later () INHERITS (alter_dep_parent);
INSERT INTO alter_dep_large SELECT generate_series(1, 100);
ANALYZE alter_dep_parent;
ANALYZE alter_dep_large;
ANALYZE alter_dep_later;
CREATE VIEW alter_dep_view AS SELECT id FROM alter_dep_later;
ALTER TABLE alter_dep_parent ALTER id TYPE bigint;

-- A foreign inheritance parent cannot hide a large physical child.
CREATE FOREIGN DATA WRAPPER alter_dummy_fdw;
CREATE SERVER alter_dummy_server FOREIGN DATA WRAPPER alter_dummy_fdw;
CREATE FOREIGN TABLE alter_foreign_parent (id integer) SERVER alter_dummy_server;
CREATE TABLE alter_foreign_child () INHERITS (alter_foreign_parent);
INSERT INTO alter_foreign_child SELECT generate_series(1, 100);
ANALYZE alter_foreign_child;
ALTER FOREIGN TABLE alter_foreign_parent ALTER id TYPE bigint;
SELECT pg_typeof(id) = 'integer'::regtype AS foreign_child_type_unchanged
FROM alter_foreign_child LIMIT 1;

-- ONLY and duplicate ALTER TYPE errors take precedence when no rewrite can run.
CREATE TABLE alter_core_errors (id integer);
CREATE TABLE alter_core_errors_child () INHERITS (alter_core_errors);
INSERT INTO alter_core_errors SELECT generate_series(1, 100);
ANALYZE alter_core_errors;
ALTER TABLE ONLY alter_core_errors ALTER id TYPE bigint;
ALTER TABLE alter_core_errors ALTER id TYPE bigint, ALTER id TYPE text USING id::text;
ALTER TABLE alter_core_errors ALTER id TYPE integer, ALTER id TYPE bigint;

-- Storage-less partitioned parents are skipped, but large leaves are checked.
CREATE TABLE alter_part (partkey integer, value integer, label text) PARTITION BY RANGE (partkey);
CREATE TABLE alter_part_leaf PARTITION OF alter_part FOR VALUES FROM (0) TO (1000);
INSERT INTO alter_part SELECT g, g, 'leaf' FROM generate_series(1, 100) AS g;
ANALYZE alter_part;
ANALYZE alter_part_leaf;
SELECT pg_relation_filenode('alter_part_leaf') AS part_leaf_before \gset
ALTER TABLE alter_part ALTER value TYPE bigint;
ALTER TABLE alter_part ALTER label TYPE varchar;
SELECT :'part_leaf_before'::oid = pg_relation_filenode('alter_part_leaf') AS part_leaf_unchanged,
       pg_typeof(label) = 'character varying'::regtype AS partition_binary_type
FROM alter_part LIMIT 1;

-- Threshold boundary, bypass, disabled mode, and multi-command atomicity.
CREATE TABLE alter_boundary (id integer);
INSERT INTO alter_boundary SELECT generate_series(1, 10);
ANALYZE alter_boundary;
ALTER TABLE alter_boundary ALTER id TYPE bigint;

CREATE TABLE alter_controls (id integer);
INSERT INTO alter_controls SELECT generate_series(1, 100);
ANALYZE alter_controls;
SELECT pg_relation_filenode('alter_controls') AS controls_heap_before \gset
ALTER TABLE alter_controls ADD COLUMN should_not_exist integer, ALTER id TYPE bigint;
SELECT count(*) = 0 AS added_column_absent,
       (SELECT atttypid = 'integer'::regtype
        FROM pg_attribute
        WHERE attrelid = 'alter_controls'::regclass AND attname = 'id') AS type_unchanged,
       :'controls_heap_before'::oid = pg_relation_filenode('alter_controls') AS heap_unchanged
FROM pg_attribute
WHERE attrelid = 'alter_controls'::regclass AND attname = 'should_not_exist' AND NOT attisdropped;
SET pg_savior.bypass = on;
ALTER TABLE alter_controls ALTER id TYPE bigint;
RESET pg_savior.bypass;
SET pg_savior.enabled = off;
ALTER TABLE alter_controls ALTER id TYPE integer;
SET pg_savior.enabled = on;
SELECT pg_typeof(id) = 'integer'::regtype AS controls_applied FROM alter_controls LIMIT 1;

SET pg_savior.bypass = on;
DROP VIEW alter_invalid_view, alter_dep_view;
DROP TABLE alter_allowed, alter_collation, alter_blocked, alter_invalid,
           alter_bad_default, alter_generated, alter_identity,
           alter_inh_child, alter_inh_parent,
           alter_dep_large, alter_dep_later, alter_dep_parent,
           alter_foreign_child;
DROP FOREIGN TABLE alter_foreign_parent;
DROP SERVER alter_dummy_server;
DROP FOREIGN DATA WRAPPER alter_dummy_fdw;
DROP TABLE alter_core_errors_child, alter_core_errors,
           alter_part, alter_boundary, alter_controls;
DROP DOMAIN alter_free_domain, alter_checked_domain;
RESET pg_savior.bypass;
