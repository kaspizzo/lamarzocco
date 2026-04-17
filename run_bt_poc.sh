#!/bin/sh

set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PYLMARZOCCO_DIR="${ROOT_DIR}/../pylamarzocco"
PYTHON_BIN="${PYLMARZOCCO_DIR}/.venv312/bin/python"

if [ ! -x "${PYTHON_BIN}" ]; then
  echo "Missing Python environment: ${PYTHON_BIN}" >&2
  echo "Create it in ../pylamarzocco first, for example:" >&2
  echo "  cd ${PYLMARZOCCO_DIR}" >&2
  echo "  uv venv --python 3.12 .venv312" >&2
  echo "  uv pip install --python .venv312/bin/python bleak aiohttp mashumaro cryptography bleak-retry-connector" >&2
  exit 1
fi

exec "${PYTHON_BIN}" "${ROOT_DIR}/bt_poc_app.py" "$@"
