#!/usr/bin/env bash
# Docker-based integration test for pg_savior.
# Builds an image with the extension installed, starts postgres,
# runs `make installcheck` inside the container, prints regression
# diff on failure.
#
# Usage:
#   docker/test.sh                       # default PG version (16)
#   PG_MAJOR=15 docker/test.sh           # test against another PG version
#   docker/test.sh --capture-expected    # copy results/*.out from container
#                                        # back to host expected/ (use once
#                                        # to seed expected outputs)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PG_MAJOR="${PG_MAJOR:-16}"
IMAGE="pg-savior-test:pg${PG_MAJOR}"
CONTAINER="pg-savior-test-pg${PG_MAJOR}"

CAPTURE_EXPECTED=0
for arg in "$@"; do
  case "$arg" in
    --capture-expected) CAPTURE_EXPECTED=1 ;;
    *) echo "unknown arg: $arg"; exit 2 ;;
  esac
done

cleanup() { docker rm -f "$CONTAINER" >/dev/null 2>&1 || true; }
trap cleanup EXIT

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
