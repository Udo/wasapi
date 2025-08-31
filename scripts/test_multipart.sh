#!/usr/bin/env bash
# Multipart form-data POST test (with file upload)
# Usage: ./test_multipart.sh [URL] [PATH]
set -euo pipefail
BASE_URL=${1:-${TEST_URL:-http://localhost/web/wasapi/examples/demo.endpoint}}
REQ_PATH=${2:-}
build_url() { local base="$1" path="$2"; if [[ -z "$path" ]]; then echo "$base"; else if [[ "$base" == */ ]]; then echo "${base%/}$path"; else echo "${base}$path"; fi; fi; }
FULL_URL=$(build_url "$BASE_URL" "$REQ_PATH")
TMP_FILE=$(mktemp /tmp/fcgi_upload_test_XXXX.txt)
trap 'rm -f "$TMP_FILE"' EXIT
printf 'Sample file upload at %s\n' "$(date -u)" > "$TMP_FILE"
echo "== MULTIPART POST to ${FULL_URL}" >&2
curl -sS -D /tmp/headers.multipart.$$ -o /tmp/body.multipart.$$ \
  -F "text_field=hello world" \
  -F "json_field={\"k\":123}" \
  -F "upload_file=@${TMP_FILE};type=text/plain" \
  "${FULL_URL}" || { echo 'curl failed' >&2; exit 1; }

echo '--- Response Headers ---'
cat /tmp/headers.multipart.$$ | sed 's/\r$//' 
echo '--- Response Body ---'
cat /tmp/body.multipart.$$ 
rm -f /tmp/headers.multipart.$$ /tmp/body.multipart.$$ || true
