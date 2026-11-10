#!/usr/bin/env bash
# Smoke test for the running server. Stage 1 checks only /api/health;
# index and search checks are added in later stages.
set -euo pipefail

BASE_URL="${BASE_URL:-http://localhost:8080}"
TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-120}"

deadline=$((SECONDS + TIMEOUT_SECONDS))
while :; do
  if body="$(curl -fsS "${BASE_URL}/api/health" 2>/dev/null)"; then
    break
  fi
  if (( SECONDS >= deadline )); then
    echo "ERROR: ${BASE_URL}/api/health did not return 200 within ${TIMEOUT_SECONDS}s" >&2
    exit 1
  fi
  sleep 2
done

echo "health: ${body}"
if ! grep -Eq '"status"[[:space:]]*:[[:space:]]*"ok"' <<<"${body}"; then
  echo 'ERROR: health response has no "status":"ok"' >&2
  exit 1
fi
if ! grep -Eq '"ollama"[[:space:]]*:[[:space:]]*(true|false)' <<<"${body}"; then
  echo 'ERROR: health response has no boolean "ollama"' >&2
  exit 1
fi
echo "smoke test passed"
