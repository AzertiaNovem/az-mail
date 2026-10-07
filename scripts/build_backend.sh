#!/usr/bin/env bash
# Release build of the AZ Mail backend, with the unit tests.
#
# Usage: scripts/build_backend.sh [--preset NAME] [--jobs N] [--skip-tests] [--out DIR]
#   --preset      CMake preset (default: linux-release on Linux, mac-release on macOS)
#   --jobs        parallel build jobs (default: CPU count)
#   --skip-tests  do not run ctest
#   --out DIR     copy the stripped `azmail` binary to DIR (e.g. a staging dir for deployment)
# The binary is backend/build/<preset>/azmail. Deployment: docs/DEPLOY.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

if [ "$(uname -s)" = "Darwin" ]; then PRESET=mac-release; else PRESET=linux-release; fi
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
TESTS=1
OUT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --preset) PRESET="$2"; shift ;;
    --jobs) JOBS="$2"; shift ;;
    --skip-tests) TESTS=0 ;;
    --out) OUT="$2"; shift ;;
    -h|--help) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

command -v cmake >/dev/null || { echo "cmake (≥ 3.24) is required" >&2; exit 1; }
BUILD_DIR="backend/build/${PRESET}"
GEN=()
if [ ! -f "${BUILD_DIR}/CMakeCache.txt" ] && command -v ninja >/dev/null; then GEN=(-G Ninja); fi

echo "==> configure (${PRESET})"
cmake --preset "${PRESET}" -S backend ${GEN[@]+"${GEN[@]}"}
echo "==> build (-j${JOBS})"
cmake --build "${BUILD_DIR}" -j "${JOBS}"
if [ "${TESTS}" = 1 ]; then
  echo "==> unit tests"
  ctest --test-dir "${BUILD_DIR}" --output-on-failure -j "${JOBS}"
fi

BIN="${BUILD_DIR}/azmail"
"${BIN}" version
if [ -n "${OUT}" ]; then
  mkdir -p "${OUT}"
  cp "${BIN}" "${OUT}/azmail"
  if command -v strip >/dev/null; then strip "${OUT}/azmail" 2>/dev/null || true; fi
  BIN="${OUT}/azmail"
fi
if command -v sha256sum >/dev/null; then sha256sum "${BIN}"; else shasum -a 256 "${BIN}"; fi
echo "==> done: ${BIN}"
