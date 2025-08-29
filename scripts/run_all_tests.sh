#!/usr/bin/env bash
# Run all curl-based tests against a base URL (server must already be running)
# Usage: ./run_all_tests.sh [URL] [PATH]
set -euo pipefail
BASE_URL=${1:-${TEST_URL:-http://127.0.0.1}}
REQ_PATH=${2:-}
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

chmod +x "$DIR"/test_*.sh || true

"$DIR"/test_json.sh "$BASE_URL" "$REQ_PATH"

echo
"$DIR"/test_urlencoded.sh "$BASE_URL" "$REQ_PATH"

echo
"$DIR"/test_multipart.sh "$BASE_URL" "$REQ_PATH"

echo
"$DIR"/test_cookies.sh "$BASE_URL" "$REQ_PATH"

echo '== ALL TESTS COMPLETED ==' >&2
