#!/usr/bin/env bash
# URL-encoded form POST test.
# Usage: ./test_urlencoded.sh [URL] [PATH]
set -euo pipefail
BASE_URL=${1:-${TEST_URL:-http://localhost/web/wasapi/examples/demo.endpoint}}
REQ_PATH=${2:-}
build_url() { local base="$1" path="$2"; if [[ -z "$path" ]]; then echo "$base"; else if [[ "$base" == */ ]]; then echo "${base%/}$path"; else echo "${base}$path"; fi; fi; }
FULL_URL=$(build_url "$BASE_URL" "$REQ_PATH")
DATA='name=Alice+Smith&age=30&skills=cpp&skills=linux'
echo "== URLENC POST to ${FULL_URL}" >&2
curl -sS -D /tmp/headers.urlencoded.$$ -o /tmp/body.urlencoded.$$ \
  -H 'Content-Type: application/x-www-form-urlencoded' \
  --data "${DATA}" \
  "${FULL_URL}" || { echo 'curl failed' >&2; exit 1; }

echo '--- Response Headers ---'
cat /tmp/headers.urlencoded.$$ | sed 's/\r$//' 
echo '--- Response Body ---'
cat /tmp/body.urlencoded.$$ 
rm -f /tmp/headers.urlencoded.$$ /tmp/body.urlencoded.$$ || true
