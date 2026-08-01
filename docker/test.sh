#!/usr/bin/env bash
# Docker-based integration test for pg_savior.
# Builds an image with the extension installed, starts postgres,
# runs `make installcheck` inside the container, prints regression
# diff on failure.
#
# Usage:
#   docker/test.sh                       # default PG version (16)
#   PG_MAJOR=15 docker/test.sh           # test against another PG version
#   TEST_ARTIFACT_DIR=artifacts docker/test.sh
#                                        # save failure diagnostics to artifacts/
#   docker/test.sh --capture-expected    # copy results/*.out from container
#                                        # back to host expected/ (use once
#                                        # to seed expected outputs)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PG_MAJOR="${PG_MAJOR:-16}"
IMAGE="pg-savior-test:pg${PG_MAJOR}"
CONTAINER="pg-savior-test-pg${PG_MAJOR}"
TEST_ARTIFACT_DIR="${TEST_ARTIFACT_DIR:-}"

CAPTURE_EXPECTED=0
for arg in "$@"; do
  case "$arg" in
    --capture-expected) CAPTURE_EXPECTED=1 ;;
    *) echo "unknown arg: $arg"; exit 2 ;;
  esac
done

# These functions are reached through the EXIT trap call chain.
# shellcheck disable=SC2329
cleanup() { docker rm -f "$CONTAINER" >/dev/null 2>&1 || true; }

# shellcheck disable=SC2329
collect_failure_diagnostics() {
  [ -n "$TEST_ARTIFACT_DIR" ] || return 0

  echo "==> Saving failure diagnostics to $TEST_ARTIFACT_DIR"
  mkdir -p "$TEST_ARTIFACT_DIR"
  {
    echo "PG_MAJOR=$PG_MAJOR"
    echo "IMAGE=$IMAGE"
    docker version 2>&1 || true
  } >"$TEST_ARTIFACT_DIR/environment.txt"

  if docker inspect "$CONTAINER" >/dev/null 2>&1; then
    docker logs "$CONTAINER" >"$TEST_ARTIFACT_DIR/postgres.log" 2>&1 || true
    docker inspect "$CONTAINER" >"$TEST_ARTIFACT_DIR/container-inspect.json" 2>&1 || true
    docker cp "$CONTAINER":/pg_savior/regression.diffs \
      "$TEST_ARTIFACT_DIR/regression.diffs" >/dev/null 2>&1 || true
    docker cp "$CONTAINER":/pg_savior/regression.out \
      "$TEST_ARTIFACT_DIR/regression.out" >/dev/null 2>&1 || true
    docker cp "$CONTAINER":/pg_savior/results \
      "$TEST_ARTIFACT_DIR/results" >/dev/null 2>&1 || true
  else
    echo "Container $CONTAINER was not created; see the job log for build output." \
      >"$TEST_ARTIFACT_DIR/no-container.txt"
  fi
}

# shellcheck disable=SC2329
on_exit() {
  local result="$?"
  if [ "$result" -ne 0 ]; then
    collect_failure_diagnostics
  fi
  cleanup
  exit "$result"
}
trap on_exit EXIT

echo "==> Building image $IMAGE"
docker build \
  --build-arg "PG_MAJOR=$PG_MAJOR" \
  -f "$SCRIPT_DIR/Dockerfile" \
  -t "$IMAGE" \
  "$SRC_DIR" || exit 1

echo "==> Starting postgres"
docker run -d \
  --name "$CONTAINER" \
  -e POSTGRES_PASSWORD=postgres \
  "$IMAGE" >/dev/null || exit 1

echo "==> Waiting for postgres to accept connections"
for i in $(seq 1 30); do
  if docker exec "$CONTAINER" pg_isready -U postgres -h localhost >/dev/null 2>&1; then
    echo "    ready after ${i}s"
    break
  fi
  if [ "$i" = 30 ]; then
    echo "    ERROR: postgres did not become ready in 30s"
    docker logs "$CONTAINER"
    exit 1
  fi
  sleep 1
done

echo "==> Running make installcheck"
docker exec -e PGUSER=postgres "$CONTAINER" \
  bash -c 'cd /pg_savior && make installcheck'
RESULT=$?

if [ "$CAPTURE_EXPECTED" = 1 ]; then
  echo "==> Copying results/*.out from container to host expected/"
  mkdir -p "$SRC_DIR/expected"
  docker cp "$CONTAINER":/pg_savior/results/. "$SRC_DIR/expected/"
  echo "    captured. Review the diffs and commit the expected/ files."
  exit 0
fi

if [ "$RESULT" -eq 0 ]; then
  echo ""
  echo "==> All tests passed"
  exit 0
fi

echo ""
echo "==> Tests failed. regression.diffs:"
docker exec "$CONTAINER" bash -c \
  'cat /pg_savior/regression.diffs 2>/dev/null || echo "(no regression.diffs file)"'
exit 1
