MODULES = pg_savior
EXTENSION = pg_savior
DATA = pg_savior--0.0.1.sql

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
