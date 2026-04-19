#!/bin/sh

set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
PYLMARZOCCO_DIR="${PYLMARZOCCO_ROOT:-${ROOT_DIR}/../pylamarzocco}"
PYTHON_BIN="${PYTHON_BIN:-${PYLMARZOCCO_DIR}/.venv312/bin/python}"

if [ ! -x "${PYTHON_BIN}" ]; then
  echo "Missing Python environment: ${PYTHON_BIN}" >&2
  echo "Point PYTHON_BIN at the pylamarzocco virtualenv or create the default one first." >&2
  echo "Expected repo: ${PYLMARZOCCO_DIR}" >&2
  exit 1
fi

export PYLMARZOCCO_ROOT="${PYLMARZOCCO_DIR}"
exec "${PYTHON_BIN}" "${ROOT_DIR}/tools/debug/cloud_dashboard_debug.py" "$@"
