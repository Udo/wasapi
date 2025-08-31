#!/usr/bin/env bash
set -euo pipefail

# WebSocket upgrade + single frame send test.
# Usage: ./test_websocket.sh [WS_URL]
# Default WS_URL uses BASE_URL or TEST_URL env; example: http://localhost/ws/web/wasapi/examples/demo.endpoint
BASE_URL=${1:-${TEST_URL:-http://localhost/ws/web/wasapi/examples/demo.endpoint}}

url="$BASE_URL"
proto="${url%%://*}"
rest="${url#*://}"
hostport="${rest%%/*}"
path="/${rest#*/}"
host="${hostport%%:*}"
port="${hostport##*:}"
if [[ "$host" == "$port" ]]; then # no explicit port
	if [[ "$proto" == http ]]; then port=80; else port=443; fi
fi

key=$(openssl rand -base64 16 | tr -d '\n')
echo "== WS Handshake to $host:$port$path" >&2

request="GET $path HTTP/1.1\r\nHost: $host\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: $key\r\nSec-WebSocket-Version: 13\r\nOrigin: http://$host\r\n\r\n"

exec 3<>/dev/tcp/$host/$port
printf "%b" "$request" >&3

# Read handshake response headers
resp=""
while IFS= read -r -u 3 line; do
	resp+="$line\n"
	[[ "$line" == $'\r' ]] && break
done
echo "--- Handshake Response ---"; echo -e "$resp" | sed 's/\r$//' 
grep -q "101 Switching Protocols" <<<"$resp" || { echo "Handshake failed" >&2; exit 1; }

# Build and send a masked text frame with payload 'Hello' using Python for correctness
python3 - <<'PY' 1>&3
import sys
payload = b"Hello"
mask = b"\x01\x02\x03\x04"
frame = bytearray()
frame.append(0x81)
frame.append(0x80 | len(payload))
frame += mask
masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
frame += masked
sys.stdout.buffer.write(frame)
sys.stdout.flush()
PY

# Try to read one frame back (simplistic; assumes unfragmented small frame)
sleep 0.2
dd bs=1 count=2 status=none <&3 > /tmp/ws_hdr.$$ || true
if [[ -s /tmp/ws_hdr.$$ ]]; then
	b1=$(hexdump -v -e '/1 "%02X"' /tmp/ws_hdr.$$ | cut -c1-2)
	b2=$(hexdump -v -e '/1 "%02X"' /tmp/ws_hdr.$$ | cut -c3-4)
	fin=$(( 0x$b1 & 0x80 ))
	opcode=$(( 0x$b1 & 0x0F ))
	masked=$(( 0x$b2 & 0x80 ))
	plen=$(( 0x$b2 & 0x7F ))
	if (( plen > 0 )); then
		dd bs=1 count=$plen status=none <&3 > /tmp/ws_payload.$$
		echo "--- Received Frame ---" >&2
		echo "FIN=$fin OPCODE=$opcode MASKED=$masked LEN=$plen" >&2
		echo -n "Payload: "
		cat /tmp/ws_payload.$$ | tr -d '\r' | head -c 256
		echo
		rm -f /tmp/ws_payload.$$
	else
		echo "(Empty frame)" >&2
	fi
else
	echo "No frame received (server callback maybe produced no reply)" >&2
fi
rm -f /tmp/ws_hdr.$$
exec 3>&-
echo "== WS test complete ==" >&2

