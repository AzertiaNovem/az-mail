#!/usr/bin/env bash
# AZ Mail local development in one command:
#   mock Resend + R2 (tools/mock_resend, webhooks looped back to the backend)
#   → azmail backend (built if missing; R2 blob store against the mock by default)
#   → demo data on the first start (scripts/seed_dev.py)
#   → Vite dev server on http://localhost:5173 (proxies /api to 127.0.0.1:8080).
# Ctrl-C stops everything. State lives in data/dev/ (git-ignored); --reset starts over.
#
# Usage: scripts/dev.sh [--reset] [--local] [--no-frontend] [--no-seed]
#   --reset        delete data/dev (database, blobs, mock R2 objects, secrets) first
#   --local        AZMAIL_BLOB_BACKEND=local instead of r2 (against the mock R2)
#   --no-frontend  backend + mock only (e.g. when running `pnpm dev` yourself)
#   --no-seed      do not seed demo data on a fresh instance
# Environment: AZMAIL_BIN (backend binary), AZMAIL_DEV_DIR, MOCK_PORT (8787), MOCK_S3_PORT (8788).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

RESET=0; BLOB_BACKEND=r2; FRONTEND=1; SEED=1
while [ $# -gt 0 ]; do
  case "$1" in
    --reset) RESET=1 ;;
    --local) BLOB_BACKEND=local ;;
    --no-frontend) FRONTEND=0 ;;
    --no-seed) SEED=0 ;;
    -h|--help) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
  esac
  shift
done

API_PORT=8080   # fixed: frontend/vite.config.ts proxies /api to 127.0.0.1:8080
MOCK_PORT="${MOCK_PORT:-8787}"
MOCK_S3_PORT="${MOCK_S3_PORT:-8788}"
DOMAIN="azmail.test"
DEV_DIR="${AZMAIL_DEV_DIR:-${ROOT}/data/dev}"
if [ "$(uname -s)" = "Darwin" ]; then PRESET=mac-debug; else PRESET=linux-debug; fi
BUILD_DIR="${ROOT}/backend/build/${PRESET}"
AZMAIL_BIN="${AZMAIL_BIN:-${BUILD_DIR}/azmail}"

log() { printf '\033[1;34m[dev]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[dev]\033[0m %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "需要 $1（$2）"; }

need python3 "Python ≥ 3.10"
port_busy() { python3 -c "import socket,sys; s=socket.socket(); sys.exit(0 if s.connect_ex(('127.0.0.1',$1))==0 else 1)"; }
for p in "${API_PORT}" "${MOCK_PORT}" "${MOCK_S3_PORT}"; do
  if port_busy "${p}"; then die "端口 ${p} 已被占用（另一个 dev.sh 还在运行？）"; fi
done

if [ "${RESET}" = 1 ]; then
  log "删除 ${DEV_DIR}"
  rm -rf "${DEV_DIR}"
fi
mkdir -p "${DEV_DIR}/data" "${DEV_DIR}/logs" "${DEV_DIR}/r2"

# ---- backend binary ---------------------------------------------------------------------------
if [ ! -x "${AZMAIL_BIN}" ]; then
  need cmake "CMake ≥ 3.24"
  log "构建后端（${PRESET}）…"
  cmake --preset "${PRESET}" -S backend >/dev/null
  cmake --build "${BUILD_DIR}" -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
fi
[ -x "${AZMAIL_BIN}" ] || die "找不到后端可执行文件 ${AZMAIL_BIN}"

# ---- secrets (generated once, kept in ${DEV_DIR}/secrets.env, mode 600) --------------------------
SECRETS="${DEV_DIR}/secrets.env"
if [ ! -f "${SECRETS}" ]; then
  umask 077
  python3 - "${SECRETS}" <<'PY'
import base64, secrets, sys
values = {
    "AZMAIL_SECRET": secrets.token_hex(32),
    "RESEND_API_KEY": "re_dev_" + secrets.token_hex(12),
    "RESEND_WEBHOOK_SECRET": "whsec_" + base64.b64encode(secrets.token_bytes(24)).decode(),
    "R2_ACCESS_KEY_ID": "dev-" + secrets.token_hex(6),
    "R2_SECRET_ACCESS_KEY": secrets.token_hex(20),
}
with open(sys.argv[1], "w") as fh:
    fh.writelines(f"{k}={v}\n" for k, v in values.items())
PY
  umask 022
fi
set -a
# shellcheck disable=SC1090
. "${SECRETS}"
set +a

# ---- backend environment (names: backend/src/config.hpp) ----------------------------------------
export AZMAIL_LISTEN_ADDRESS=127.0.0.1
export AZMAIL_PORT="${API_PORT}"
export AZMAIL_PUBLIC_API_URL="http://localhost:5173"   # signed file URLs go through the Vite proxy
export AZMAIL_CORS_ORIGINS="http://localhost:5173,http://127.0.0.1:5173"
export AZMAIL_DATA_DIR="${DEV_DIR}/data"
export AZMAIL_DB_PATH="${DEV_DIR}/data/azmail.db"
export AZMAIL_LOCAL_DOMAINS="${DOMAIN}"
export AZMAIL_LOG_LEVEL="${AZMAIL_LOG_LEVEL:-debug}"
export AZMAIL_ALLOW_INSECURE_HTTP=1                     # http:// mock endpoints
export AZMAIL_POLL_INTERVAL_SEC=60
export RESEND_API_BASE="http://127.0.0.1:${MOCK_PORT}"
export AZMAIL_BLOB_BACKEND="${BLOB_BACKEND}"
export R2_ACCOUNT_ID=devaccount
export R2_BUCKET=azmail-dev
export R2_ENDPOINT="http://127.0.0.1:${MOCK_S3_PORT}"
export R2_PREFIX=azmail/
export AZMAIL_FILES_DELIVERY="${AZMAIL_FILES_DELIVERY:-proxy}"

FIRST_RUN=0
[ -f "${AZMAIL_DB_PATH}" ] || FIRST_RUN=1

# ---- process management ---------------------------------------------------------------------------
PIDS=()
cleanup() {
  local rc=$?
  trap - EXIT INT TERM
  log "停止所有进程…"
  for pid in ${PIDS[@]+"${PIDS[@]}"}; do kill "${pid}" 2>/dev/null || true; done
  for pid in ${PIDS[@]+"${PIDS[@]}"}; do wait "${pid}" 2>/dev/null || true; done
  exit "${rc}"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

wait_http() {  # url, seconds, name, log[, pid]
  local i=0
  while [ "${i}" -lt "$(( $2 * 5 ))" ]; do
    if [ -n "${5:-}" ] && ! kill -0 "$5" 2>/dev/null; then
      tail -n 30 "$4" >&2 || true
      die "$3 已退出（日志：$4）"
    fi
    if python3 -c "import urllib.request,sys; urllib.request.urlopen(sys.argv[1], timeout=1)" "$1" 2>/dev/null; then
      return 0
    fi
    sleep 0.2; i=$(( i + 1 ))
  done
  tail -n 30 "$4" >&2 || true
  die "$3 没有在 $2 秒内就绪（日志：$4）"
}

log "启动模拟 Resend/R2：http://127.0.0.1:${MOCK_PORT}（R2 :${MOCK_S3_PORT}）"
PYTHONPATH="${ROOT}${PYTHONPATH:+:${PYTHONPATH}}" python3 -m tools.mock_resend.server \
  --port "${MOCK_PORT}" --s3-port "${MOCK_S3_PORT}" --api-key "${RESEND_API_KEY}" \
  --webhook-url "http://127.0.0.1:${API_PORT}/api/webhooks/resend" --webhook-secret "${RESEND_WEBHOOK_SECRET}" \
  --local-domains "${DOMAIN}" --s3-access-key "${R2_ACCESS_KEY_ID}" --s3-secret-key "${R2_SECRET_ACCESS_KEY}" \
  --s3-bucket "${R2_BUCKET}" --s3-data-dir "${DEV_DIR}/r2" >>"${DEV_DIR}/logs/mock.log" 2>&1 &
MOCK_PID=$!
PIDS+=("${MOCK_PID}")
wait_http "http://127.0.0.1:${MOCK_PORT}/_mock/health" 10 "模拟服务器" "${DEV_DIR}/logs/mock.log" "${MOCK_PID}"

"${AZMAIL_BIN}" migrate >>"${DEV_DIR}/logs/backend.log" 2>&1 || { tail -n 20 "${DEV_DIR}/logs/backend.log" >&2; die "azmail migrate 失败"; }
log "启动后端：http://127.0.0.1:${API_PORT}（blob: ${BLOB_BACKEND}）"
"${AZMAIL_BIN}" serve >>"${DEV_DIR}/logs/backend.log" 2>&1 &
BACKEND_PID=$!
PIDS+=("${BACKEND_PID}")
wait_http "http://127.0.0.1:${API_PORT}/api/health" 30 "后端" "${DEV_DIR}/logs/backend.log" "${BACKEND_PID}"

if [ "${FIRST_RUN}" = 1 ] && [ "${SEED}" = 1 ]; then
  log "首次启动：写入演示数据（scripts/seed_dev.py）…"
  if ! python3 scripts/seed_dev.py --api "http://127.0.0.1:${API_PORT}" --mock "http://127.0.0.1:${MOCK_PORT}" \
       --azmail-bin "${AZMAIL_BIN}" --domain "${DOMAIN}" --credentials "${DEV_DIR}/credentials.txt"; then
    log "演示数据写入失败（后端功能尚未完成？）；可稍后 --reset 重试"
  fi
fi
[ -f "${DEV_DIR}/credentials.txt" ] && log "账号密码见 ${DEV_DIR}/credentials.txt"
log "日志：${DEV_DIR}/logs/{backend,mock}.log    模拟服务器控制台：http://127.0.0.1:${MOCK_PORT}/_mock/state"

if [ "${FRONTEND}" = 1 ]; then
  need pnpm "Node ≥ 22 + pnpm"
  if [ ! -d frontend/node_modules ]; then log "安装前端依赖…"; (cd frontend && pnpm install); fi
  log "启动前端：http://localhost:5173 （Ctrl-C 结束）"
  (cd frontend && exec pnpm dev) &
  VITE_PID=$!
  PIDS+=("${VITE_PID}")
  wait "${VITE_PID}"
else
  log "运行中（Ctrl-C 结束）"
  wait "${BACKEND_PID}"
fi
