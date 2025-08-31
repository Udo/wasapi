#ifndef WEBSOCKETS_H
#define WEBSOCKETS_H

#include <vector>
#include <cstdint>
#include <functional>
#include "request.h"

namespace ws
{
	using RequestReadyCallback = void (*)(Request&, std::vector<uint8_t>& out_payload);
	int serve(int port, const std::string& unix_socket, RequestReadyCallback cb);
}

#endif // WEBSOCKETS_H
