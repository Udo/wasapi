#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include "dynamic_variable.h"

struct GlobalConfig
{
	uint16_t port = 9000;
	std::string unix_path;
	int backlog = 256 * 16;

	size_t arena_capacity = 256 * 1024;
	size_t workers = 1;
	size_t output_buffer_initial = 32 * 1024;

	std::string upload_tmp_dir = "/tmp";

	uint32_t max_in_flight = 64;
	size_t max_params_bytes = 256 * 1024;
	size_t max_stdin_bytes = 2 * 1024 * 1024;

	size_t body_preview_limit = 1024;
	size_t print_env_limit = 0;
	size_t print_params_limit = 0;
	int print_indent = 2;
	bool pretty_print_params = true;
	int params_json_depth = -1;

	std::string endpoint_file_path = "SCRIPT_FILENAME";

	std::string default_content_type = "text/plain; charset=utf-8";

	std::string session_cookie_name = "session_id";
	double session_cookie_lifetime = 60 * 60 * 24 * 30; // 30 days
	std::string session_cookie_path = "/";
	std::string session_storage_path = "/tmp/sessions";
	bool session_auto_load = true;

	std::string http_cookies_var = "HTTP_COOKIE";
	std::string http_query_var = "QUERY_STRING";

	bool keep_uploaded_files = false;
	bool cleanup_temp_on_disconnect = true;

	int log_level = 1;
	std::string log_destination = "stderr";

	size_t graceful_shutdown_timeout_ms = 5000;
};

extern GlobalConfig global_config;

bool config_parse_args(int argc, char* argv[], std::vector<std::string>& errors);

bool load_kv_file(const std::string& path, DynamicVariable& out);

#endif
