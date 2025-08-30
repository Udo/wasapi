#include "fileio.h"
#include "config.h"
#include <sys/stat.h>
#include <fstream>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <chrono>

struct CachedFile
{
	std::string content;
	time_t mtime;
	std::chrono::steady_clock::time_point last_check;
	size_t size;

	CachedFile() : mtime(0), size(0) {}
	CachedFile(const std::string& content, time_t mtime)
		: content(content), mtime(mtime), last_check(std::chrono::steady_clock::now()), size(content.size()) {}
};

static std::unordered_map<std::string, CachedFile> file_cache;
static std::mutex cache_mutex;
static std::atomic<uint32_t> call_counter{ 0 };
static size_t total_cache_size = 0;

static void evict_old_cache_entries()
{
	auto now = std::chrono::steady_clock::now();
	auto ttl_duration = std::chrono::duration<double>(global_config.file_cache_ttl);

	auto it = file_cache.begin();
	while (it != file_cache.end())
	{
		auto age = std::chrono::duration_cast<std::chrono::duration<double>>(now - it->second.last_check);
		if (age > ttl_duration)
		{
			total_cache_size -= it->second.size;
			it = file_cache.erase(it);
		}
		else
		{
			++it;
		}
	}
}

static void evict_by_size()
{
	while (total_cache_size > global_config.file_cache_max_size && !file_cache.empty())
	{
		auto oldest_it = file_cache.begin();
		for (auto it = file_cache.begin(); it != file_cache.end(); ++it)
		{
			if (it->second.last_check < oldest_it->second.last_check)
			{
				oldest_it = it;
			}
		}

		total_cache_size -= oldest_it->second.size;
		file_cache.erase(oldest_it);
	}
}

std::string read_entire_file_cached(const std::string& filename)
{
	uint32_t current_call = call_counter.fetch_add(1) + 1;

	auto now = std::chrono::steady_clock::now();

	{
		std::lock_guard<std::mutex> lock(cache_mutex);

		if (current_call % 10 == 0)
		{
			evict_old_cache_entries();
			evict_by_size();
		}

		auto it = file_cache.find(filename);
		if (it != file_cache.end())
		{
			auto time_since_check = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.last_check);
			if (time_since_check.count() < 1)
			{
				return it->second.content;
			}
		}
	}

	struct stat st;
	if (stat(filename.c_str(), &st) != 0)
	{
		return "";
	}

	time_t current_mtime = st.st_mtime;

	{
		std::lock_guard<std::mutex> lock(cache_mutex);

		auto it = file_cache.find(filename);
		if (it != file_cache.end())
		{
			it->second.last_check = now;

			if (it->second.mtime == current_mtime)
			{
				return it->second.content;
			}
			total_cache_size -= it->second.size;
			file_cache.erase(it);
		}

		std::ifstream file(filename, std::ios::binary);
		if (!file)
		{
			return "";
		}

		file.seekg(0, std::ios::end);
		size_t size = file.tellg();
		file.seekg(0, std::ios::beg);

		if (size > global_config.file_cache_max_size)
		{
			std::string content(size, '\0');
			file.read(&content[0], size);
			if (!file)
			{
				return "";
			}
			return content;
		}

		std::string content(size, '\0');
		file.read(&content[0], size);

		if (!file)
		{
			return "";
		}

		total_cache_size += size;
		file_cache[filename] = CachedFile(content, current_mtime);

		evict_by_size();

		return content;
	}
}

bool write_entire_file(const std::string& filename, const std::string& content)
{
	std::ofstream file(filename, std::ios::binary | std::ios::trunc);
	if (!file)
	{
		return false;
	}

	file.write(content.data(), content.size());
	if (!file)
	{
		return false;
	}

	file.close();

	struct stat st;
	if (stat(filename.c_str(), &st) != 0)
	{
		std::lock_guard<std::mutex> lock(cache_mutex);
		auto it = file_cache.find(filename);
		if (it != file_cache.end())
		{
			total_cache_size -= it->second.size;
			file_cache.erase(it);
		}
		return true;
	}

	time_t new_mtime = st.st_mtime;

	{
		std::lock_guard<std::mutex> lock(cache_mutex);

		if (content.size() > global_config.file_cache_max_size)
		{
			auto it = file_cache.find(filename);
			if (it != file_cache.end())
			{
				total_cache_size -= it->second.size;
				file_cache.erase(it);
			}
		}
		else
		{
			auto it = file_cache.find(filename);
			if (it != file_cache.end())
			{
				total_cache_size -= it->second.size;
			}

			total_cache_size += content.size();
			file_cache[filename] = CachedFile(content, new_mtime);

			evict_by_size();
		}
	}

	return true;
}

CacheStats get_cache_stats()
{
	std::lock_guard<std::mutex> lock(cache_mutex);
	return {
		file_cache.size(),
		total_cache_size,
		global_config.file_cache_max_size
	};
}