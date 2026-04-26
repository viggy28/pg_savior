# pg_savior

pg_savior is a PostgreSQL extension that prevents accidental data loss from `DELETE` and `UPDATE` statements that have no `WHERE` clause. It hooks the parser, raises an `ERROR` on unguarded statements, and aborts the transaction so the application notices.

## Status

Under active development. Pre-1.0. Not production-ready.

## Features

- [x] Block `DELETE` without `WHERE`
- [x] Block `UPDATE` without `WHERE`
- [x] Row-count threshold guard (`pg_savior.max_rows_affected`)
- [x] Per-session bypass GUC (`pg_savior.bypass`)
- [x] Global on/off GUC (`pg_savior.enabled`)
- [ ] Block destructive DDL (`DROP TABLE`, `TRUNCATE`, `DROP DATABASE`, `CREATE INDEX` without `CONCURRENTLY`)
- [ ] Per-table opt-out via reloptions

## Installation

Build from source, or [download from PGXN](https://pgxn.org/extension/pg_savior).

```bash
make
sudo make install
```

### Activate the protection

`CREATE EXTENSION` alone does **not** activate pg_savior. The shared library must be loaded into Postgres backends. Pick one:

**Option 1 — Cluster-wide (recommended for production)**

Add to `postgresql.conf`:

```
shared_preload_libraries = 'pg_savior'
```

Then restart Postgres. Every backend forked from the postmaster will have the hook installed automatically.

**Option 2 — Per-session, no restart**

Add to `postgresql.conf`:

```
session_preload_libraries = 'pg_savior'
```

Then `SELECT pg_reload_conf();`. Every new connection from then on installs the hook.

**Option 3 — Per-session, manual (development)**

```sql
LOAD 'pg_savior';
```

Once loaded by any of the above, register the extension in each database:

```sql
CREATE EXTENSION pg_savior;
```

## Usage

```
postgres=# CREATE EXTENSION pg_savior;
CREATE EXTENSION

postgres=# CREATE TABLE emp (id int);
CREATE TABLE

postgres=# INSERT INTO emp VALUES (1), (2), (3);
INSERT 0 3

postgres=# DELETE FROM emp;
ERROR:  pg_savior: DELETE without WHERE clause is blocked
HINT:  Add a WHERE clause, or set pg_savior.bypass = on for this session.

postgres=# SELECT count(*) FROM emp;
 count
-------
     3
(1 row)

postgres=# DELETE FROM emp WHERE id = 1;
DELETE 1
```

## Configuration

| GUC | Default | Scope | Effect |
|---|---|---|---|
| `pg_savior.enabled` | `on` | session (`USERSET`) | Master switch. When `off`, no checks run. |
| `pg_savior.bypass` | `off` | session (`USERSET`) | When `on`, the current session's `DELETE`/`UPDATE` are allowed through unconditionally. Use to do an intentional bulk operation. |
| `pg_savior.max_rows_affected` | `0` (disabled) | session (`USERSET`) | When > 0, refuse `DELETE`/`UPDATE` whose planner row estimate exceeds this. Catches destructive queries that *do* have a `WHERE` but match too much (e.g. `DELETE FROM emp WHERE id > 0`). |

Example bypass for an intentional cleanup:

```sql
BEGIN;
SET LOCAL pg_savior.bypass = on;
DELETE FROM staging_table;
COMMIT;
```

Example row-count guard for a destructive query that has a WHERE but matches too much:

```sql
postgres=# SET pg_savior.max_rows_affected = 100;
SET
postgres=# DELETE FROM emp WHERE id > 0;
ERROR:  pg_savior: DELETE estimated to affect 1000 rows, exceeds pg_savior.max_rows_affected (100)
HINT:  Refine the WHERE clause, raise pg_savior.max_rows_affected, or set pg_savior.bypass = on. Run ANALYZE if the estimate looks wrong.
```

The threshold uses the planner's row estimate, which depends on table statistics. For accurate enforcement on a recently-modified table, run `ANALYZE` first.

## Tests

The extension uses `pg_regress` (the standard PGXS test framework). Run against a local cluster:

```bash
make installcheck
```

Each test file uses `LOAD 'pg_savior'` so the framework works whether or not `pg_savior` is in `shared_preload_libraries`.

### Docker-based integration test

A self-contained integration test that builds Postgres + pg_savior in a container and runs the suite end-to-end:

```bash
./docker/test.sh
```

Test against a different Postgres major version:

```bash
PG_MAJOR=15 ./docker/test.sh
```

If you change a test's SQL, regenerate its expected output:

```bash
# clear stale expected file, leave an empty placeholder so pg_regress
# runs the test instead of bailing out, then capture
> expected/<testname>.out
./docker/test.sh --capture-expected
```

## How it works

pg_savior installs two hooks:

1. **`post_parse_analyze_hook`** — fires after parse-analyze, before planning. Inspects the `Query` tree: if the statement is `CMD_DELETE`/`CMD_UPDATE` and `query->jointree->quals` is `NULL` (no `WHERE`), it raises `ERROR`. Independent of plan shape; parameterized statements handled correctly; no planner work wasted on a query that will be refused.

2. **`ExecutorStart_hook`** — fires after planning, before execution. If `pg_savior.max_rows_affected > 0`, reads the planner's row estimate from the source plan beneath the `ModifyTable` node and raises `ERROR` if it exceeds the threshold. The transaction aborts before any tuples are touched.

Both checks honour `pg_savior.enabled` and `pg_savior.bypass`.
