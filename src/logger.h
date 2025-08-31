#ifndef LOGGER_H
#define LOGGER_H
#include <string>
#include <cstdio>
#include <cstdarg>
#include <mutex>
#include "config.h"

inline std::mutex& log_mutex()
{
	static std::mutex m;
	return m;
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
inline void log(int level, const char* fmt, ...)
{
	if (level > global_config.log_level)
		return;
	std::lock_guard<std::mutex> lk(log_mutex());
	FILE* out = log_destination();
	va_list ap;
	va_start(ap, fmt);
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
	std::fputs("[ERROR] ", out);
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
	std::fputs("[INFO] ", out);
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
	std::fputs("[DEBUG] ", out);
	std::vfprintf(out, fmt, ap);
	std::fputc('\n', out);
	va_end(ap);
}

#endif
