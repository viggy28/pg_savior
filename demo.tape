# VHS tape — generates demo.gif for the README.
# Regenerate with:  vhs demo.tape
#
# Prerequisite: a Docker container with pg_savior installed and
# session-preloaded. From the repo root:
#
#   docker build -f docker/Dockerfile -t pg-savior-demo .
#   docker run -d --name pg_savior_demo \
#     -e POSTGRES_PASSWORD=postgres \
#     pg-savior-demo \
#     -c session_preload_libraries=pg_savior
#
# Then:   vhs demo.tape
# Cleanup: docker rm -f pg_savior_demo

Output demo.gif

Set FontSize 18
Set Width 1100
Set Height 400
Set Theme "Dracula"
Set Padding 20
Set TypingSpeed 45ms

# Hidden setup: open psql with the large-table threshold lowered to 2 (via
# PGOPTIONS so we don't have to SET it in the visible session), create and
# populate a 3-row table, ANALYZE so pg_class.reltuples reflects reality.
# Then clear the screen so the recorded portion starts blank.
Hide
Type "docker exec -it -e PGOPTIONS='-c pg_savior.large_table_threshold_rows=2' pg_savior_demo psql -U postgres"
Enter
Sleep 1500ms
Type "CREATE EXTENSION IF NOT EXISTS pg_savior;"
Enter
Sleep 400ms
Type "DROP TABLE IF EXISTS emp;"
Enter
Sleep 400ms
Type "CREATE TABLE emp (id int);"
Enter
Sleep 400ms
Type "INSERT INTO emp VALUES (1), (2), (3);"
Enter
Sleep 400ms
Type "ANALYZE emp;"
Enter
Sleep 400ms
Ctrl+L
Sleep 300ms
Show

# Disaster #1: DELETE without WHERE
Sleep 500ms
Type "-- oops, forgot the WHERE clause"
Enter
Sleep 600ms
Type "DELETE FROM emp;"
Enter
Sleep 3s

# Disaster #2: DROP TABLE on what turns out to be a "large" table
Type "-- wrong database — this is prod"
Enter
Sleep 600ms
Type "DROP TABLE emp;"
Enter
Sleep 3s

Type "\q"
Enter
Sleep 500ms
