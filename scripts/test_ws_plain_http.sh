#!/usr/bin/env bash
# Plain HTTP request sent to the WebSocket endpoint (no Upgrade header)
# Verifies that the server treats it as normal HTTP and returns a response.
# Usage: ./test_ws_plain_http.sh [URL] [METHOD]
set -euo pipefail
BASE_URL=${1:-${TEST_URL:-http://localhost/ws/web/wasapi/examples/demo.endpoint}}
METHOD=${2:-GET}
BODY='{"ping":"pong"}'
if [[ "$METHOD" == POST ]]; then
  echo "== HTTP POST over WS port to $BASE_URL" >&2
  curl -sS -D /tmp/ws_http_headers.$$ -o /tmp/ws_http_body.$$ -X POST -H 'Content-Type: application/json' --data "$BODY" "$BASE_URL" || { echo 'curl failed' >&2; exit 1; }
else
  echo "== HTTP GET over WS port to $BASE_URL" >&2
  curl -sS -D /tmp/ws_http_headers.$$ -o /tmp/ws_http_body.$$ -X GET "$BASE_URL" || { echo 'curl failed' >&2; exit 1; }
fi
echo '--- Response Headers ---'
cat /tmp/ws_http_headers.$$ | sed 's/\r$//'
status=$(head -n1 /tmp/ws_http_headers.$$ | awk '{print $2}')
if [[ -z "${status}" || "${status}" -lt 200 || "${status}" -ge 400 ]]; then
  echo "Unexpected HTTP status: $status" >&2
  cat /tmp/ws_http_body.$$ >&2
  exit 1
fi
echo '--- Response Body (first 500 bytes) ---'
head -c 500 /tmp/ws_http_body.$$; echo
rm -f /tmp/ws_http_headers.$$ /tmp/ws_http_body.$$ || true
echo '== WS plain HTTP test complete ==' >&2
