#ifndef REQUEST_H
#define REQUEST_H

#include <cstdint>
#include <string>
#include "dynamic_variable.h"

struct Request
{
	uint16_t id = 0;

	enum RequestFlags : uint64_t
	{
		INITIALIZED = 1ULL << 0, // request has been initialized
		KEEP_CONNECTION = 1ULL << 1, // keep connection alive after response
		PARAMS_COMPLETE = 1ULL << 2, // all parameters received
		INPUT_COMPLETE = 1ULL << 3, // all input data received
		RESPONDED = 1ULL << 4, // response has been sent
		ABORTED = 1ULL << 5, // request was aborted
		FAILED = 1ULL << 6, // request failed (limit or protocol failure)
	};
	uint64_t flags = 0;

	DynamicVariable env;
	DynamicVariable params;
	DynamicVariable cookies;
	DynamicVariable headers;
	DynamicVariable files;
	DynamicVariable session;
	DynamicVariable context;
	std::string session_id;
	std::string body;
	size_t params_bytes = 0;
	size_t body_bytes = 0;
};

#endif
