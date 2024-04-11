create table saved_queries
(
    id serial primary key,
    query text not null,
    executed_at timestamp not null default now()
);

CREATE FUNCTION
show_saved() RETURNS table
(
    id integer,
    query text,
    executed_at timestamp
) AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;