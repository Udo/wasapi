#include "config.h"
#include "fileio.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <cctype>
#include <functional>

GlobalConfig global_config;

bool config_parse_args(int argc, char* argv[], std::vector<std::string>& errors)
{
	struct Opt
	{
		const char* name;
		bool needs;
		std::function<void(const char*)> apply;
	};
	std::vector<Opt> opts = {
		Opt{ "--port", true, [](const char* v)
			 { global_config.port = (uint16_t)std::stoi(v); } },
		Opt{ "--unix", true, [](const char* v)
			 { global_config.unix_path = v; } },
		Opt{ "--backlog", true, [](const char* v)
			 { global_config.backlog = std::stoi(v); } },
		Opt{ "--max-in-flight", true, [](const char* v)
			 { global_config.max_in_flight = (uint32_t)std::stoul(v); } },
		Opt{ "--max-params", true, [](const char* v)
			 { global_config.max_params_bytes = (size_t)std::stoull(v); } },
		Opt{ "--max-stdin", true, [](const char* v)
			 { global_config.max_stdin_bytes = (size_t)std::stoull(v); } },
		Opt{ "--arena-capacity", true, [](const char* v)
			 { global_config.arena_capacity = (size_t)std::stoull(v); } },
		Opt{ "--output-buffer", true, [](const char* v)
			 { global_config.output_buffer_initial = (size_t)std::stoull(v); } },
		Opt{ "--upload-tmp", true, [](const char* v)
			 { global_config.upload_tmp_dir = v; } },
		Opt{ "--body-preview", true, [](const char* v)
			 { global_config.body_preview_limit = (size_t)std::stoull(v); } },
		Opt{ "--print-env-limit", true, [](const char* v)
			 { global_config.print_env_limit = (size_t)std::stoull(v); } },
		Opt{ "--print-params-limit", true, [](const char* v)
			 { global_config.print_params_limit = (size_t)std::stoull(v); } },
		Opt{ "--print-indent", true, [](const char* v)
			 { global_config.print_indent = std::stoi(v); } },
		Opt{ "--params-json-depth", true, [](const char* v)
			 { global_config.params_json_depth = std::stoi(v); } },
		Opt{ "--keep-uploads", false, [](const char*)
			 { global_config.keep_uploaded_files = true; } },
		Opt{ "--no-cleanup-temp", false, [](const char*)
			 { global_config.cleanup_temp_on_disconnect = false; } },
		Opt{ "--log-level", true, [](const char* v)
			 { global_config.log_level = std::stoi(v); } },
		Opt{ "--log-dest", true, [](const char* v)
			 { global_config.log_destination = v; } },
		Opt{ "--help", false, [](const char*) {} },
	};
	auto find_opt = [&](const std::string& a) -> Opt*
	{ for (auto& o : opts) if (a == o.name) return &o; return nullptr; };
	for (int i = 1; i < argc; ++i)
	{
		std::string a = argv[i];
		Opt* o = find_opt(a);
		if (!o)
		{
			errors.push_back("Unknown arg: " + a);
			continue;
		}
		if (a == "--help")
			return false;
		const char* val = nullptr;
		if (o->needs)
		{
			if (i + 1 >= argc)
			{
				errors.push_back("Missing value for " + a);
				continue;
			}
			val = argv[++i];
		}
		o->apply(val);
	}
	return errors.empty();
}

static inline void trim_inplace(std::string& s)
{
	size_t start = 0;
	while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
		++start;
	size_t end = s.size();
	while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
		--end;
	if (start == 0 && end == s.size())
		return;
	s.assign(s, start, end - start);
}

bool load_kv_file(const std::string& path, DynamicVariable& out)
{
	if (out.type != DynamicVariable::OBJECT)
		out = DynamicVariable::make_object();

	std::string content = read_entire_file_cached(path);
	if (content.empty())
		return false;

	std::istringstream in(content);
	std::string line;
	std::string last_key = "undefined";
	size_t lineno = 0;
	while (std::getline(in, line))
	{
		++lineno;
		std::string key;
		std::string value;
		trim_inplace(line);
		if (line.empty())
			continue;
		if (line[0] == '#' || line[0] == ';')
			continue;
		auto eq = line.find('=');
		if (eq == std::string::npos)
		{
			key = last_key;
			value = line;
		}
		else
		{
			key = line.substr(0, eq);
			value = line.substr(eq + 1);
		}
		trim_inplace(key);
		trim_inplace(value);
		DynamicVariable* existing = out.find(key);
		if (!existing)
		{
			out[key] = DynamicVariable::make_string(value);
		}
		else
		{
			if (existing->type == DynamicVariable::STRING)
			{
				std::string prev = existing->data.s;
				*existing = DynamicVariable::make_array();
				existing->push(DynamicVariable::make_string(prev));
				existing->push(DynamicVariable::make_string(value));
			}
			else if (existing->type == DynamicVariable::ARRAY)
			{
				existing->push(DynamicVariable::make_string(value));
			}
			else
			{
				*existing = DynamicVariable::make_string(value);
			}
		}
		last_key = key;
	}
	return true;
}
