#ifndef LOGGER_H
#define LOGGER_H
#include <string>
#include <cstdio>
#include <cstdarg>
//#include <mutex>
#include "config.h"

inline FILE* log_destination()
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
inline void log(int level, const char* fmt, ...)
{
	if (level > global_config.log_level)
		return;
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
	FILE* out = log_destination();
	va_list ap;
	va_start(ap, fmt);
	std::fputs("[DEBUG] ", out);
	std::vfprintf(out, fmt, ap);
	std::fputc('\n', out);
	va_end(ap);
}

#endif
