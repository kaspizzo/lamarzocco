#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET="${IDF_TARGET_OVERRIDE:-esp32s3}"
COMMAND="${1:-quick}"

if [[ $# -gt 0 ]]; then
  shift
fi

ensure_idf() {
  local export_script=""

  if [[ -n "${IDF_PATH:-}" && -f "${IDF_PATH}/export.sh" ]]; then
    export_script="${IDF_PATH}/export.sh"
  elif [[ -f "${HOME}/esp/esp-idf/export.sh" ]]; then
    export_script="${HOME}/esp/esp-idf/export.sh"
  fi

  if [[ -n "${export_script}" ]]; then
    # shellcheck disable=SC1090
    source "${export_script}" >/dev/null
    return
  fi

  if ! command -v idf.py >/dev/null 2>&1; then
    echo "ESP-IDF nicht gefunden. Setze IDF_PATH oder installiere ~/esp/esp-idf." >&2
    exit 1
  fi
}

detect_port() {
  if [[ -n "${ESPPORT:-}" ]]; then
    echo "${ESPPORT}"
    return
  fi

  local patterns=(
    "/dev/cu.usbmodem*"
    "/dev/cu.usbserial*"
    "/dev/cu.SLAB_USBtoUART*"
    "/dev/cu.wchusbserial*"
  )
  local pattern
  local candidate

  shopt -s nullglob
  for pattern in "${patterns[@]}"; do
    for candidate in ${pattern}; do
      if [[ -e "${candidate}" ]]; then
        echo "${candidate}"
        shopt -u nullglob
        return
      fi
    done
  done
  shopt -u nullglob

  echo "Kein serieller Port gefunden. Setze ESPPORT=/dev/cu...." >&2
  exit 1
}

ensure_target() {
  local current_target=""

  if [[ -f "${ROOT_DIR}/sdkconfig" ]]; then
    current_target="$(sed -n 's/^CONFIG_IDF_TARGET=\"\(.*\)\"/\1/p' "${ROOT_DIR}/sdkconfig" | head -n 1)"
  fi

  if [[ "${current_target}" != "${TARGET}" ]]; then
    (cd "${ROOT_DIR}" && idf.py set-target "${TARGET}" >/dev/null)
  fi
}

run_idf() {
  (cd "${ROOT_DIR}" && idf.py "$@")
}

ensure_idf
ensure_target

case "${COMMAND}" in
  build)
    run_idf build "$@"
    ;;
  flash)
    PORT="$(detect_port)"
    echo "Using port ${PORT}"
    run_idf -p "${PORT}" flash "$@"
    ;;
  quick)
    PORT="$(detect_port)"
    echo "Using port ${PORT}"
    run_idf -p "${PORT}" app-flash monitor "$@"
    ;;
  full)
    PORT="$(detect_port)"
    echo "Using port ${PORT}"
    run_idf -p "${PORT}" flash monitor "$@"
    ;;
  monitor)
    PORT="$(detect_port)"
    echo "Using port ${PORT}"
    run_idf -p "${PORT}" monitor "$@"
    ;;
  erase)
    PORT="$(detect_port)"
    echo "Using port ${PORT}"
    run_idf -p "${PORT}" erase-flash "$@"
    ;;
  clean)
    run_idf fullclean "$@"
    ;;
  menuconfig)
    run_idf menuconfig "$@"
    ;;
  ports)
    ls /dev/cu.* 2>/dev/null || true
    ;;
  *)
    cat <<'EOF'
Usage: ./dev.sh [build|flash|quick|full|monitor|erase|clean|menuconfig|ports]

  build       Build only
  flash       Full flash without monitor
  quick       Fast loop: app-flash + monitor
  full        First flash: full flash + monitor
  monitor     Open serial monitor
  erase       Erase flash
  clean       Run idf.py fullclean
  menuconfig  Open ESP-IDF menuconfig
  ports       List serial ports

Set ESPPORT=/dev/cu.usbmodemXXXX to override port detection.
EOF
    exit 1
    ;;
esac
