#!/usr/bin/env bash
# Copies a frontend build to the web root and never overwrites the environment's /config.js.
#
# Usage: scripts/deploy_frontend.sh [--api-base URL] [--dist DIR] [--root DIR] [--dry-run]
#   --api-base URL  write <root>/config.js with this API origin (e.g. https://mail-api.example.com);
#                   required on the first install, optional afterwards (the existing file is kept)
#   --dist DIR      build output to deploy (default: frontend/dist, see scripts/build_frontend.sh)
#   --root DIR      web root of deploy/nginx-frontend.conf (default: /var/www/azmail)
#   --dry-run       show what rsync would change; write nothing
# Example: sudo scripts/deploy_frontend.sh --api-base https://mail-api.example.com   (first install)
#          sudo scripts/deploy_frontend.sh                                          (upgrades)
set -euo pipefail

# Why: every build contains the dev default config.js (apiBase "" = same origin). Copying it over
# the environment's file makes the SPA call /api/... on the static site (index.html / 405), which
# locks every user out. rsync --exclude=/config.js protects the file from both overwrite and
# --delete; a "protect" filter alone would still overwrite it.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="${ROOT}/frontend/dist"
WEBROOT="/var/www/azmail"
API_BASE=""
DRY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --api-base) [ $# -ge 2 ] || { echo "--api-base needs a value" >&2; exit 2; }; API_BASE="$2"; shift ;;
    --dist) [ $# -ge 2 ] || { echo "--dist needs a value" >&2; exit 2; }; DIST="$2"; shift ;;
    --root) [ $# -ge 2 ] || { echo "--root needs a value" >&2; exit 2; }; WEBROOT="$2"; shift ;;
    --dry-run) DRY=1 ;;
    -h|--help) sed -n '2,11p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

DIST="${DIST%/}"
WEBROOT="${WEBROOT%/}"
[ -n "${WEBROOT}" ] || { echo "--root must not be /" >&2; exit 2; }
[ -f "${DIST}/index.html" ] || { echo "no build in ${DIST} (run scripts/build_frontend.sh first)" >&2; exit 1; }
command -v rsync >/dev/null || { echo "rsync is required" >&2; exit 1; }

if [ -n "${API_BASE}" ]; then
  API_BASE="${API_BASE%/}"
  # An origin only (scheme://host[:port]); the value is pasted into JavaScript, so nothing else.
  if ! printf '%s' "${API_BASE}" | grep -Eq '^https?://([A-Za-z0-9-]+(\.[A-Za-z0-9-]+)*|\[[0-9A-Fa-f:.]+\])(:[0-9]{1,5})?$'; then
    echo "--api-base must be an http(s) origin without a path, e.g. https://mail-api.example.com" >&2
    exit 2
  fi
  case "${API_BASE}" in
    http://*) echo "warning: --api-base uses http:// (production must be https://)" >&2 ;;
  esac
elif [ ! -f "${WEBROOT}/config.js" ]; then
  echo "${WEBROOT}/config.js does not exist: pass --api-base https://<API domain> on the first install" >&2
  exit 1
elif grep -Eq 'apiBase:[[:space:]]*""' "${WEBROOT}/config.js"; then
  echo "warning: ${WEBROOT}/config.js has apiBase \"\" (same origin); with a separate API domain" \
       "pass --api-base to fix it" >&2
fi

if [ "${DRY}" = 1 ]; then
  rsync -a --delete --exclude=/config.js --dry-run --itemize-changes "${DIST}/" "${WEBROOT}/"
  [ -z "${API_BASE}" ] || echo "would write ${WEBROOT}/config.js (apiBase ${API_BASE})"
  exit 0
fi

mkdir -p "${WEBROOT}"
if [ -n "${API_BASE}" ]; then
  # Written before the copy (rsync never touches it) and atomically, so no request sees half a file.
  TMP="$(mktemp "${WEBROOT}/.config.js.XXXXXX")"
  cat > "${TMP}" <<EOF
// AZ Mail runtime configuration for this environment (scripts/deploy_frontend.sh).
window.__AZMAIL_CONFIG__ = { apiBase: "${API_BASE}" };
EOF
  chmod 0644 "${TMP}"
  mv -f "${TMP}" "${WEBROOT}/config.js"
  echo "==> wrote ${WEBROOT}/config.js (apiBase ${API_BASE})"
fi
rsync -a --delete --exclude=/config.js "${DIST}/" "${WEBROOT}/"
echo "==> deployed ${DIST} → ${WEBROOT} (config.js kept)"
