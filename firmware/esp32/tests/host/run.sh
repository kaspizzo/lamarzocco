#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HOST_DIR="${ROOT_DIR}/tests/host"
BUILD_DIR="${HOST_DIR}/build"
CC_BIN="${CC:-cc}"
OUTPUT="${BUILD_DIR}/host_tests"
CJSON_DIR="${ROOT_DIR}/managed_components/espressif__cjson/cJSON"

if [[ ! -f "${CJSON_DIR}/cJSON.c" ]]; then
  echo "cJSON source not found at ${CJSON_DIR}/cJSON.c" >&2
  echo "Run ./dev.sh build once so managed components are available." >&2
  exit 1
fi

mkdir -p "${BUILD_DIR}"

"${CC_BIN}" \
  -std=c11 \
  -Wall \
  -Wextra \
  -Werror \
  -Wno-unused-function \
  -Wno-unused-parameter \
  -I"${HOST_DIR}/include" \
  -I"${ROOT_DIR}/main" \
  -I"${CJSON_DIR}" \
  "${HOST_DIR}/src/test_runner.c" \
  "${HOST_DIR}/src/test_idf_stubs.c" \
  "${HOST_DIR}/src/test_nvs.c" \
  "${HOST_DIR}/src/test_httpd.c" \
  "${HOST_DIR}/src/test_controller_state.c" \
  "${HOST_DIR}/src/test_controller_settings.c" \
  "${HOST_DIR}/src/test_cloud_api.c" \
  "${HOST_DIR}/src/test_storage_security.c" \
  "${HOST_DIR}/src/test_setup_portal_page.c" \
  "${HOST_DIR}/src/test_setup_portal_routes.c" \
  "${ROOT_DIR}/main/controller_state.c" \
  "${ROOT_DIR}/main/cloud_api.c" \
  "${ROOT_DIR}/main/setup_portal_http.c" \
  "${ROOT_DIR}/main/setup_portal_page.c" \
  "${ROOT_DIR}/main/setup_portal_routes_shared.c" \
  "${CJSON_DIR}/cJSON.c" \
  -o "${OUTPUT}"

"${OUTPUT}" "$@"
