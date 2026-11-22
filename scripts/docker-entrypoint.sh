#!/usr/bin/env bash
# Container entrypoint: make sure a corpus exists, then start the server.
#
# `docker compose up` must work from a clean clone, so the image generates a
# deterministic corpus into the mounted volume on the first start. The
# generator validates manifest.json and reuses an existing corpus, which keeps
# later starts fast and never overwrites a corpus the user prepared.
set -euo pipefail

DATASET_PROFILE="${DATASET_PROFILE:-full}"
DATA_DIR="${DATA_DIR:-/data/generated/${DATASET_PROFILE}}"
export DATA_DIR
GENERATE_DATASET="${GENERATE_DATASET:-true}"

if [ "${GENERATE_DATASET}" = "true" ]; then
  args=(--output "${DATA_DIR}" --profile "${DATASET_PROFILE}")
  if [ -n "${DATASET_SEED:-}" ]; then
    args+=(--seed "${DATASET_SEED}")
  fi
  if [ "${DATASET_VERIFY_HASH:-false}" = "true" ]; then
    args+=(--verify-hash)
  fi

  echo "dataset: ensuring the ${DATASET_PROFILE} corpus in ${DATA_DIR}"
  started="$(date +%s)"
  python3 /app/tools/generate_dataset.py "${args[@]}"
  echo "dataset: ready after $(( $(date +%s) - started ))s"
else
  echo "dataset: generation disabled, using ${DATA_DIR} as-is"
fi

exec /app/search_server
