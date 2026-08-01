LOAD 'pg_savior';
CREATE EXTENSION IF NOT EXISTS pg_savior;

-- Updatable views follow the explicit-WHERE policy.
CREATE TABLE view_base (id int PRIMARY KEY);
INSERT INTO view_base VALUES (1), (2), (3);
CREATE VIEW all_rows_view AS SELECT * FROM view_base;
CREATE VIEW filtered_view AS SELECT * FROM view_base WHERE id <= 2;

DELETE FROM all_rows_view;
SELECT count(*) AS rows_after_view_block FROM view_base;

-- A predicate in the view definition is not an explicit predicate in the DELETE.
DELETE FROM filtered_view;
SELECT count(*) AS rows_after_filtered_view_block FROM view_base;

DELETE FROM all_rows_view WHERE id = 1 RETURNING id;
SELECT array_agg(id ORDER BY id) AS after_view_delete FROM view_base;

-- Partitioned parents and individual partitions are both protected.
CREATE TABLE part_emp (id int, value int) PARTITION BY RANGE (id);
CREATE TABLE part_emp_low PARTITION OF part_emp FOR VALUES FROM (0) TO (100);
CREATE TABLE part_emp_high PARTITION OF part_emp FOR VALUES FROM (100) TO (200);
INSERT INTO part_emp VALUES (1, 10), (2, 20), (101, 30);

DELETE FROM part_emp;
SELECT count(*) AS rows_after_partition_parent_block FROM part_emp;

DELETE FROM part_emp WHERE id = 1 RETURNING id;
SELECT count(*) AS rows_after_partition_delete FROM part_emp;

DELETE FROM part_emp_high;
SELECT count(*) AS rows_after_direct_partition_block FROM part_emp_high;

-- Traditional inheritance has the same behavior, including ONLY.
CREATE TABLE inherit_parent (id int, value int);
CREATE TABLE inherit_child () INHERITS (inherit_parent);
INSERT INTO inherit_parent VALUES (1, 10);
INSERT INTO inherit_child VALUES (2, 20);

UPDATE inherit_parent SET value = 0;
SELECT count(*) AS unchanged_inheritance_rows
FROM inherit_parent WHERE value > 0;

UPDATE ONLY inherit_parent SET value = 11 WHERE id = 1 RETURNING id;
SELECT value AS parent_value FROM ONLY inherit_parent WHERE id = 1;
SELECT value AS child_value FROM inherit_child WHERE id = 2;

DROP VIEW filtered_view;
DROP VIEW all_rows_view;
DROP TABLE view_base;
DROP TABLE part_emp;
DROP TABLE inherit_child;
DROP TABLE inherit_parent;
