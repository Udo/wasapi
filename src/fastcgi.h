#ifndef FASTCGI_H
#define FASTCGI_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <unordered_map>
#include "http.h"
#include "dynamic_variable.h"
#include "request.h"

namespace fcgi
{

	enum Type : uint8_t
	{
		FCGI_BEGIN_REQUEST = 1,
		FCGI_ABORT_REQUEST = 2,
		FCGI_END_REQUEST = 3,
		FCGI_PARAMS = 4,
		FCGI_STDIN = 5,
		FCGI_STDOUT = 6,
		FCGI_STDERR = 7,
		FCGI_DATA = 8,
		FCGI_GET_VALUES = 9,
		FCGI_GET_VALUES_RESULT = 10,
		FCGI_UNKNOWN_TYPE = 11
	};

	enum Role : uint16_t
	{
		RESPONDER = 1,
		AUTHORIZER = 2,
		FILTER = 3
	};

	enum Flags : uint8_t
	{
		KEEP_CONN = 1
	};

	enum ProtocolStatus : uint8_t
	{
		REQUEST_COMPLETE = 0,
		CANT_MPX_CONN = 1,
		OVERLOADED = 2,
		UNKNOWN_ROLE = 3
	};
	static const uint8_t VERSION_1 = 1;

	struct Header
	{
		uint8_t version;
		uint8_t type;
		uint16_t requestId;
		uint16_t contentLength;
		uint8_t paddingLength;
		uint8_t reserved;
	} __attribute__((packed));

	struct BeginRequestBody
	{
		uint16_t role;
		uint8_t flags;
		uint8_t reserved[5];
	} __attribute__((packed));

	struct EndRequestBody
	{
		uint32_t appStatus;
		uint8_t protocolStatus;
		uint8_t reserved[3];
	} __attribute__((packed));

	enum ProcessStatus
	{
		OK = 0,
		CLOSE = 1
	};

	ProcessStatus process_buffer(std::vector<uint8_t>& in_buf, std::unordered_map<uint16_t, Request*>& requests, std::vector<uint8_t>& out_buf, Request* (*allocate_request)(uint16_t), size_t max_params_bytes, size_t max_stdin_bytes, void (*on_request_ready)(Request&, std::vector<uint8_t>& out_buf), bool& waiting_for_arena);

	void append_record(std::vector<uint8_t>& out, uint8_t type, uint16_t reqId, const uint8_t* data, uint16_t len);
	void append_stdout_text(std::vector<uint8_t>& out, uint16_t reqId, const std::string& body);
	void append_end_request(std::vector<uint8_t>& out, uint16_t reqId, uint32_t appStatus, uint8_t protoStatus);

}

#endif
