#ifndef WEBSOCKETS_H
#define WEBSOCKETS_H

#include <vector>
#include <cstdint>
#include <functional>
#include "request.h"

namespace ws
{
	using RequestReadyCallback = void (*)(Request&, std::vector<uint8_t>& out_payload);
		// Serve a websocket (and plain HTTP) endpoint.
		// cbws: called for each websocket message (text/binary). out_payload becomes response frame payload (same opcode as inbound for simplicity).
		// cbhttp: called once per plain HTTP request received on this port (non-upgrade). out_payload is treated as the HTTP body and wrapped in a 200 OK response.
		int serve(int port, const std::string& unix_socket, RequestReadyCallback cbws, RequestReadyCallback cbhttp);
}

#endif // WEBSOCKETS_H
