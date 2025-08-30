#ifndef REQUEST_H
#define REQUEST_H

#include <cstdint>
#include <string>
#include "dynamic_variable.h"

struct Request
{
	uint16_t id = 0;
	
	// Request state flags - single 64-bit field with enum-defined bit positions
	enum RequestFlags : uint64_t
	{
		INITIALIZED     = 1ULL << 0,  // request has been initialized
		KEEP_CONNECTION = 1ULL << 1,  // keep connection alive after response
		PARAMS_COMPLETE = 1ULL << 2,  // all parameters received
		INPUT_COMPLETE  = 1ULL << 3,  // all input data received
		RESPONDED       = 1ULL << 4,  // response has been sent
		ABORTED         = 1ULL << 5,  // request was aborted
		FAILED          = 1ULL << 6,  // request failed (limit or protocol failure)
		// 57 more flag positions available (bits 7-63)
	};
	uint64_t flags = 0;
	
	DynamicVariable env; // environment parameters (object)
	DynamicVariable params; // query + form parameters (object)
	DynamicVariable cookies; // parsed cookies (object of key -> string)
	DynamicVariable headers; // response headers
	DynamicVariable files; // uploaded files (array of objects)
	DynamicVariable session; // session data (object)
	DynamicVariable context; // endpoint context data
	std::string session_id; // session identifier
	std::string body; // request body data (binary-safe)
	size_t params_bytes = 0; // cumulative parameter bytes received
	size_t body_bytes = 0; // cumulative body bytes received
};

#endif
