#!/usr/bin/env bash
# Simple JSON POST test.
# Usage: ./test_json.sh [URL] [PATH]
# URL defaults to http://127.0.0.1:9000
# PATH defaults to /
set -euo pipefail
BASE_URL=${1:-${TEST_URL:-http://127.0.0.1}}
REQ_PATH=${2:-}
build_url() { local base="$1" path="$2"; if [[ -z "$path" ]]; then echo "$base"; else if [[ "$base" == */ ]]; then echo "${base%/}$path"; else echo "${base}$path"; fi; fi; }
FULL_URL=$(build_url "$BASE_URL" "$REQ_PATH")
JSON='{"alpha":1,"beta":"two","nested":{"x":10,"y":[1,2,3]}}'
echo "== JSON POST to ${FULL_URL}" >&2
curl -sS -D /tmp/headers.json.$$ -o /tmp/body.json.$$ \
  -H 'Content-Type: application/json' \
  -X POST \
  --data "${JSON}" \
  "${FULL_URL}" || { echo 'curl failed' >&2; exit 1; }

echo '--- Response Headers ---'
cat /tmp/headers.json.$$ | sed 's/\r$//' 
echo '--- Response Body (raw) ---'
cat /tmp/body.json.$$ 
rm -f /tmp/headers.json.$$ /tmp/body.json.$$ || true
