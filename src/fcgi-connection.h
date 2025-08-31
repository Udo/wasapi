
#ifndef FCGI_CONNECTION_H
#define FCGI_CONNECTION_H

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <functional>
#include "request.h"

struct epoll_event;

	namespace fcgi_conn
	{
		using RequestReadyCallback = void (*)(Request&, std::vector<uint8_t>& out_buf);

		int serve(int port, const std::string& unix_socket, RequestReadyCallback cb);
	}

#endif // FCGI_CONNECTION_H
