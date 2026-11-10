#!/usr/bin/env bash
# Smoke test for the running server: health, index build, stats and search.
# Prints only counters and timings, never document text.
set -euo pipefail

BASE_URL="${BASE_URL:-http://localhost:8080}"
TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-120}"
BUILD_TIMEOUT_SECONDS="${BUILD_TIMEOUT_SECONDS:-900}"

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

echo "building index (this reads the whole dataset)..."
index_body="$(curl -fsS --max-time "${BUILD_TIMEOUT_SECONDS}" \
  -X POST "${BASE_URL}/api/index" \
  -H 'Content-Type: application/json' -d '{"threads": 2}')"
echo "index: ${index_body}"
if ! grep -Eq '"status"[[:space:]]*:[[:space:]]*"ok"' <<<"${index_body}"; then
  echo 'ERROR: index response has no "status":"ok"' >&2
  exit 1
fi
if ! grep -Eq '"documents_indexed"[[:space:]]*:[[:space:]]*[0-9]+' <<<"${index_body}"; then
  echo 'ERROR: index response has no integer "documents_indexed"' >&2
  exit 1
fi

stats_body="$(curl -fsS "${BASE_URL}/api/stats")"
echo "stats: ${stats_body}"
documents="$(sed -n 's/.*"documents"[[:space:]]*:[[:space:]]*\([0-9]\+\).*/\1/p' <<<"${stats_body}")"
if [[ -z "${documents}" || "${documents}" -le 0 ]]; then
  echo 'ERROR: stats reports no documents' >&2
  exit 1
fi

search_body="$(curl -fsS -X POST "${BASE_URL}/api/search" \
  -H 'Content-Type: application/json' \
  -d '{"query": "whale", "mode": "keyword"}')"
if ! grep -Eq '"results"[[:space:]]*:[[:space:]]*\[' <<<"${search_body}"; then
  echo 'ERROR: search response has no "results" array' >&2
  exit 1
fi
results_count="$(grep -o '"doc_id"' <<<"${search_body}" | wc -l)"
time_ms="$(sed -n 's/.*"time_ms"[[:space:]]*:[[:space:]]*\([0-9]\+\).*/\1/p' <<<"${search_body}")"
if [[ -z "${time_ms}" ]]; then
  echo 'ERROR: search response has no integer "time_ms"' >&2
  exit 1
fi
echo "search: results=${results_count} time_ms=${time_ms}"

echo "smoke test passed"
