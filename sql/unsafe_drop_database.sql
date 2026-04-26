LOAD 'pg_savior';
CREATE EXTENSION IF NOT EXISTS pg_savior;

-- Setup: ensure the test database is gone, then create it. Do both with
-- bypass on so we don't hit the guard during setup.
SET pg_savior.bypass = on;
DROP DATABASE IF EXISTS pg_savior_drop_test_db;
CREATE DATABASE pg_savior_drop_test_db;
RESET pg_savior.bypass;

-- Default: DROP DATABASE always blocked
DROP DATABASE pg_savior_drop_test_db;

-- DROP DATABASE IF EXISTS: still blocked (we refuse before standard handler decides)
DROP DATABASE IF EXISTS pg_savior_drop_test_db;

-- Bypass: succeeds
SET pg_savior.bypass = on;
DROP DATABASE pg_savior_drop_test_db;
RESET pg_savior.bypass;

-- Disabled: also allowed
SET pg_savior.bypass = on;
CREATE DATABASE pg_savior_drop_test_db;
RESET pg_savior.bypass;
SET pg_savior.enabled = off;
DROP DATABASE pg_savior_drop_test_db;
SET pg_savior.enabled = on;
