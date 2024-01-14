# pg_savior

pg_savior is a PostgreSQL extension designed to prevent accidental data loss due to non-parameterized DELETE queries without a WHERE clause.

## Features

- Detects DELETE queries without a WHERE clause.
- Logs detailed information about detected queries.
- Hooks into the PostgreSQL query execution process.

## Installation

1. Clone the repository.
2. Navigate to the repository directory.
3. Run `make` to build the extension.
4. Run `make install` to install the extension.

## Warning

This is not ready for production yet. It's a WIP and there cases where it blocks delete statements even if there is a "where" condition.

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
