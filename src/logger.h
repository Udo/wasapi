#ifndef LOGGER_H
#define LOGGER_H
#include <string>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include <thread>
#include <pthread.h>
#include <unordered_map>
#include "config.h"

extern std::unordered_map<std::thread::id, std::string> thread_names; // defined in logger.cpp

inline std::mutex& thread_names_mutex()
{
	static std::mutex m;
	return m;
}

inline std::mutex& log_mutex()
{
	static std::mutex m;
	return m;
}

inline void register_thread_name(const std::string& name)
{
	std::lock_guard<std::mutex> lk(thread_names_mutex());
	thread_names[std::this_thread::get_id()] = name;
}

inline FILE* log_destination_unsafe_init()
{
	static FILE* f = nullptr;
	static bool init = false;
	if (!init)
	{
		init = true;
		auto& d = global_config.log_destination;
		if (d == "stderr" || d.empty())
			f = stderr;
		else
		{
			f = std::fopen(d.c_str(), "a");
			if (!f)
				f = stderr;
		}
	}
	return f ? f : stderr;
}

inline FILE* log_destination()
{
	return log_destination_unsafe_init(); // call only while holding log_mutex
}

inline const char* current_thread_tag()
{
	static thread_local std::string tag;
	if (!tag.empty())
		return tag.c_str();
	{
		std::lock_guard<std::mutex> lk(thread_names_mutex());
		auto it = thread_names.find(std::this_thread::get_id());
		if (it != thread_names.end())
			tag = it->second;
	}
	if (tag.empty())
	{
		char buf[32];
		unsigned long long tid = (unsigned long long)pthread_self();
		std::snprintf(buf, sizeof(buf), "tid:%llx", tid);
		tag = buf;
	}
	return tag.c_str();
}

inline void log(int level, const char* fmt, ...)
{
	if (level > global_config.log_level)
		return;
	std::lock_guard<std::mutex> lk(log_mutex());
	FILE* out = log_destination();
	va_list ap;
	va_start(ap, fmt);
	std::fprintf(out, "[LOG%d] [%s] ", level, current_thread_tag());
	std::vfprintf(out, fmt, ap);
	std::fputc('\n', out);
	va_end(ap);
}

inline void log_error(const char* fmt, ...)
{
	if (0 > global_config.log_level)
		return; // always allow >=0
	std::lock_guard<std::mutex> lk(log_mutex());
	FILE* out = log_destination();
	va_list ap;
	va_start(ap, fmt);
	std::fprintf(out, "[ERROR] [%s] ", current_thread_tag());
	std::vfprintf(out, fmt, ap);
	std::fputc('\n', out);
	va_end(ap);
}

inline void log_info(const char* fmt, ...)
{
	if (1 > global_config.log_level)
		return;
	std::lock_guard<std::mutex> lk(log_mutex());
	FILE* out = log_destination();
	va_list ap;
	va_start(ap, fmt);
	std::fprintf(out, "[INFO] [%s] ", current_thread_tag());
	std::vfprintf(out, fmt, ap);
	std::fputc('\n', out);
	va_end(ap);
}

inline void log_debug(const char* fmt, ...)
{
	if (2 > global_config.log_level)
		return;
	std::lock_guard<std::mutex> lk(log_mutex());
	FILE* out = log_destination();
	va_list ap;
	va_start(ap, fmt);
	std::fprintf(out, "[DEBUG] [%s] ", current_thread_tag());
	std::vfprintf(out, fmt, ap);
	std::fputc('\n', out);
	va_end(ap);
}

#endif
