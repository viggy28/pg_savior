# pg_savior

pg_savior is a PostgreSQL extension that prevents accidental data loss from `DELETE` and `UPDATE` statements that have no `WHERE` clause. It hooks the parser, raises an `ERROR` on unguarded statements, and aborts the transaction so the application notices.

## Status

Under active development. Pre-1.0. Not production-ready.

## Features

- [x] Block `DELETE` without `WHERE`
- [x] Block `UPDATE` without `WHERE`
- [x] Per-session bypass GUC (`pg_savior.bypass`)
- [x] Global on/off GUC (`pg_savior.enabled`)
- [ ] Block destructive DDL (`DROP TABLE`, `TRUNCATE`, `DROP DATABASE`, `CREATE INDEX` without `CONCURRENTLY`)
- [ ] Row-count threshold guard (`pg_savior.max_rows_affected`)
- [ ] Per-table opt-out via reloptions

## Installation

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
| `pg_savior.bypass` | `off` | session (`USERSET`) | When `on`, the current session's `DELETE`/`UPDATE` without `WHERE` are allowed through. Use to do an intentional bulk operation. |

Example bypass for an intentional cleanup:

```sql
BEGIN;
SET LOCAL pg_savior.bypass = on;
DELETE FROM staging_table;
COMMIT;
```

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

pg_savior installs a `post_parse_analyze_hook`. After Postgres parses a statement and resolves names against the catalog, the hook inspects the `Query` tree. If the statement is a `CMD_DELETE` or `CMD_UPDATE` and `query->jointree->quals` is `NULL` (no `WHERE` clause), it calls `ereport(ERROR, ...)`, which aborts the transaction.

This intercepts *before* planning runs, so:
- The check is independent of plan shape (no special-casing of `IndexScan`, `HashJoin`, etc.)
- Parameterized statements are handled correctly
- No planner work is wasted on a query that will be refused
