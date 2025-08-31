#!/usr/bin/env bash
# Basic WebSocket handshake test using curl.
# Verifies HTTP 101 response and Sec-WebSocket-Accept correctness.
# Usage:
#  ./scripts/test_websocket.sh               # uses defaults (localhost + ws_port from env or 9001)
#  WS_PORT=9100 ./scripts/test_websocket.sh  # override port via env var
#  ./scripts/test_websocket.sh unix:/run/ws.sock   # test unix domain socket path
#  ./scripts/test_websocket.sh host:port            # explicit host:port
#
# Requires: curl, openssl
set -euo pipefail

HOST_PORT="localhost:${WS_PORT:-9001}"
UNIX_SOCK=""
TARGET=""
if [[ $# -gt 0 ]]; then
  if [[ $1 == unix:* ]]; then
    UNIX_SOCK="${1#unix:}"
  elif [[ $1 == */* ]]; then
    # treat as raw unix path (no prefix)
    UNIX_SOCK="$1"
  elif [[ $1 == *:* ]]; then
    HOST_PORT="$1"
  fi
fi

if [[ -n "$UNIX_SOCK" ]]; then
  TARGET="http://localhost/"  # curl ignores host for --unix-socket
else
  TARGET="http://${HOST_PORT}/"
fi

if ! command -v curl >/dev/null; then
  echo "[FAIL] curl not found" >&2; exit 2; fi
if ! command -v openssl >/dev/null; then
  echo "[FAIL] openssl not found" >&2; exit 2; fi

# Generate Sec-WebSocket-Key (16 random bytes base64)
SEC_KEY=$(openssl rand -base64 16)
EXPECT_ACCEPT=$(printf '%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11' "$SEC_KEY" | openssl sha1 -binary | openssl base64)

CURL_ARGS=( -s -i --max-time 3 -H "Connection: Upgrade" -H "Upgrade: websocket" \
  -H "Sec-WebSocket-Version: 13" -H "Sec-WebSocket-Key: ${SEC_KEY}" )
if [[ -n "$UNIX_SOCK" ]]; then
  CURL_ARGS+=( --unix-socket "$UNIX_SOCK" )
fi

RESP=$(curl "${CURL_ARGS[@]}" "$TARGET" || true)
STATUS=$(printf '%s' "$RESP" | awk 'NR==1{print $2}')
ACCEPT=$(printf '%s' "$RESP" | awk -F': ' 'BEGIN{IGNORECASE=1}/^Sec-WebSocket-Accept:/{gsub("\r","",$2);print $2}')

PASS=true
if [[ "$STATUS" != 101 ]]; then
  echo "[FAIL] Expected status 101, got '$STATUS'"; PASS=false; fi
if [[ "$ACCEPT" != "$EXPECT_ACCEPT" ]]; then
  echo "[FAIL] Sec-WebSocket-Accept mismatch"; echo " expected: $EXPECT_ACCEPT"; echo "   actual: $ACCEPT"; PASS=false; fi

if $PASS; then
  echo "[PASS] WebSocket handshake ok (${UNIX_SOCK:+unix:$UNIX_SOCK}${UNIX_SOCK:+=}$HOST_PORT)"
else
  echo "----- Raw Response Begin -----"; printf '%s\n' "$RESP"; echo "----- Raw Response End -----"; exit 1
fi

# Optional: attempt to keep connection open briefly to ensure no immediate close.
# Not robust (curl exits after handshake); this is just a sanity check.
exit 0
