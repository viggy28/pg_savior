LOAD 'pg_savior';
CREATE EXTENSION pg_savior;

SELECT count(*) AS installed FROM pg_extension WHERE extname = 'pg_savior';

SHOW pg_savior.enabled;
SHOW pg_savior.bypass;
