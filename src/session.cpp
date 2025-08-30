#include "session.h"
#include "fileio.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <random>
#include <chrono>
#include <fstream>

static std::string session_path(const std::string& id)
{
	std::string dir = global_config.session_storage_path;
	if (!dir.empty() && dir.back() != '/')
		dir.push_back('/');
	return dir + id + ".json";
}

static bool mkdir_if_not_exists(const std::string& dir)
{
	struct stat st{};
	if (::stat(dir.c_str(), &st) == 0)
	{
		if (S_ISDIR(st.st_mode))
			return true;
		return false;
	}
	if (::mkdir(dir.c_str(), 0777) == 0)
		return true;
	return false;
}

static std::string random_hex(size_t bytes)
{
	std::random_device rd;
	std::mt19937_64 gen(rd());
	std::uniform_int_distribution<uint64_t> dist(0, UINT64_MAX);
	std::string out;
	out.reserve(bytes * 2);
	while (bytes > 0)
	{
		uint64_t v = dist(gen);
		size_t chunk = bytes > sizeof(v) ? sizeof(v) : bytes;
		for (size_t i = 0; i < chunk; ++i)
		{
			unsigned b = (v >> (i * 8)) & 0xFF;
			static const char* hex = "0123456789abcdef";
			out.push_back(hex[b >> 4]);
			out.push_back(hex[b & 0xF]);
		}
		bytes -= chunk;
	}
	return out;
}

std::string session_get_id(Request& r, bool create)
{
	if (!r.session_id.empty())
		return r.session_id;
	if (!create)
		return std::string();
	r.session_id = random_hex(16);
	return r.session_id;
}

bool session_load(Request& r)
{
	if (r.session_id.empty())
		return false;

	std::string content = read_entire_file_cached(session_path(r.session_id));
	if (content.empty())
		return false;

	DynamicVariable parsed;
	size_t err = 0;
	if (parse_json(content, parsed, &err))
	{
		r.session = parsed;
		return true;
	}
	return false;
}

bool session_start(Request& r)
{
	session_get_id(r, true);
	if (!r.cookies.find(global_config.session_cookie_name))
		r.headers["Set-Cookie"] = global_config.session_cookie_name + "=" + r.session_id + "; Path=/; HttpOnly";
	if (!session_load(r))
		r.session.clear();
	return true;
}

bool session_save(Request& r)
{
	if (r.session_id.empty())
		return false;
	mkdir_if_not_exists(global_config.session_storage_path);

	std::string content = to_json(r.session, false, 0);
	std::string final_path = session_path(r.session_id);

	return write_entire_file(final_path, content);
}

bool session_clear(Request& r)
{
	if (!r.session_id.empty())
	{
		std::string path = session_path(r.session_id);
		::unlink(path.c_str());
	}
	r.session_id.clear();
	r.session = DynamicVariable::make_object();
	return true;
}
