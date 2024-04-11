# pg_savior

pg_savior is a PostgreSQL extension designed to prevent accidental data loss due to non-parameterized DELETE queries without a WHERE clause. It hooks into the query execution path.

## Warning

The extension is currently under active development. This project is not ready for production yet.

## Features
- [x] Detect DELETE queries without a WHERE clause
- [x] Detect DELETE queries which use index scan
- [x] Detects DELETE queries which uses CTE & subqueries
- [ ] Prevent CREATE INDEX without concurrently
- [ ] Prevent DROP DATABASE command

## Installation using [PGXS](https://www.postgresql.org/docs/current/extend-pgxs.html)

1. Clone the repository.
2. Navigate to the repository directory.
3. Run `make` to build the extension.
4. Run `sudo make install` to install the extension.

## Installation directly using C compiler

1. cc -I$(pg_config --includedir) -c pg_savior.c && cc -bundle -flat_namespace -undefined suppress -o pg_savior.dylib pg_savior.o
2. sudo cp pg_savior.dylib $(pg_config --libdir)

## Demo

```
$ psql -U visi -d postgres psql
(14.9 (Homebrew))
Type "help" for help.

postgres=#
postgres=# create extension pg_savior;
CREATE EXTENSION

postgres=# select * from emp;
id
----
(0 rows)

postgres=# insert into emp values(1);
INSERT 0 1

postgres=# delete from emp;
INFO: pg_savior: DELETE statement detected
INFO: pg_savior: WHERE clause is mandatory for a DELETE statement
DELETE 0

postgres=# select * from emp;
id
----
 1
(1 row)
```

## Usage

After installing the extension, you can enable it in your PostgreSQL database with the following command:

```sql
CREATE EXTENSION pg_savior;
```

## Test

The test suite of pg_savior uses [pg_yregres](https://github.com/omnigres/omnigres/tree/master/pg_yregress). To run a specific test, 

```
pg_yregress tests/D0001.yml
TAP version 14
1..4
# Initializing instances
# Subtest: initialize instance `default`
    1..2
    ok 1 - create extension pg_savior
    ok 2 - create table emp ( id int )
ok 1 - initialize instance `default`
INFO:  pg_savior: ExecutorStart_hook called
INFO:  pg_savior: ExecutorStart called
INFO:  pg_savior: queryDesc->sourceText: select oid, typname from pg_type
# Done initializing instances
ok 2 - D0001: pg_savior: Check if pg_savior is installed
# Subtest: D0001: pg_savior: delete without where clause
    1..6
    ok 1 - insert into emp values (1)
    ok 2 - insert into emp values (2)
    ok 3 - insert into emp values (3)
    ok 4 - select count(*) from emp as count
    ok 5 - delete from emp
    ok 6 - select count(*) from emp as count
ok 3 - D0001: pg_savior: delete without where clause
```