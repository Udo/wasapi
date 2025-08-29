#!/usr/bin/env bash
# Cookie handling test
# Usage: ./test_cookies.sh [URL] [PATH]
set -euo pipefail
BASE_URL=${1:-${TEST_URL:-http://127.0.0.1}}
REQ_PATH=${2:-}
build_url() { local base="$1" path="$2"; if [[ -z "$path" ]]; then echo "$base"; else if [[ "$base" == */ ]]; then echo "${base%/}$path"; else echo "${base}$path"; fi; fi; }
FULL_URL=$(build_url "$BASE_URL" "$REQ_PATH")
echo "== COOKIE GET to ${FULL_URL}" >&2
curl -sS -D /tmp/headers.cookies.$$ -o /tmp/body.cookies.$$ \
  -H 'Cookie: session_id=abc123; theme=dark; flag_only' \
  "${FULL_URL}" || { echo 'curl failed' >&2; exit 1; }

echo '--- Response Headers ---'
cat /tmp/headers.cookies.$$ | sed 's/\r$//' 
echo '--- Response Body ---'
cat /tmp/body.cookies.$$ 
rm -f /tmp/headers.cookies.$$ /tmp/body.cookies.$$ || true
