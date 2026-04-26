MODULES = pg_savior
EXTENSION = pg_savior
DATA = pg_savior--0.1.0.sql
REGRESS = basic delete_block update_block bypass disabled max_rows unsafe_create_index unsafe_add_column unsafe_drop_table unsafe_drop_database unsafe_truncate unsafe_alter_column_type

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)