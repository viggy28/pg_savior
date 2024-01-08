MODULES = pg_savior
EXTENSION = pg_savior
DATA = pg_savior--1.0.0.sql

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)