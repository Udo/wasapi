
#include "fileio.h"
#include "config.h"
#include <sys/stat.h>
#include <fstream>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <ctime>
#include <algorithm>

struct CachedFile
{
	std::string content;
	time_t mtime{};
	std::chrono::steady_clock::time_point last_check{ std::chrono::steady_clock::now() };
	size_t size{};
};

static std::unordered_map<std::string, CachedFile> file_cache;
static std::mutex cache_mutex;
static std::atomic<uint32_t> call_counter{ 0 };
static size_t total_cache_size = 0;


static inline void remove_entry_unlocked(const std::string& filename)
{
	auto it = file_cache.find(filename);
	if (it != file_cache.end())
	{
		total_cache_size -= it->second.size;
		file_cache.erase(it);
	}
}

static inline void insert_entry_unlocked(const std::string& filename, std::string&& content, time_t mtime)
{
	remove_entry_unlocked(filename);
	CachedFile cf; // avoid double lookup & temporary default entry
	cf.mtime = mtime;
	cf.last_check = std::chrono::steady_clock::now();
	cf.content = std::move(content);
	cf.size = cf.content.size();
	total_cache_size += cf.size;
	file_cache.emplace(filename, std::move(cf));
}

static inline void evict_ttl_unlocked()
{
	const auto now = std::chrono::steady_clock::now();
	const auto ttl_duration = std::chrono::duration<double>(global_config.file_cache_ttl);
	for (auto it = file_cache.begin(); it != file_cache.end();)
	{
		const auto age = now - it->second.last_check;
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

static inline void evict_size_unlocked()
{
	while (total_cache_size > global_config.file_cache_max_size && !file_cache.empty())
	{
		auto oldest = std::min_element(
			file_cache.begin(), file_cache.end(), [](auto& a, auto& b)
			{ return a.second.last_check < b.second.last_check; }
		);
		total_cache_size -= oldest->second.size;
		file_cache.erase(oldest);
	}
}

static inline void maybe_maintain_unlocked(uint32_t call_count)
{
	if (call_count % 10 == 0)
	{
		evict_ttl_unlocked();
		evict_size_unlocked();
	}
}

static bool read_file_binary(const std::string& filename, std::string& out, size_t known_size = static_cast<size_t>(-1))
{
	if (known_size != static_cast<size_t>(-1))
	{
		std::ifstream file(filename, std::ios::binary);
		if (!file)
			return false;
		out.resize(known_size);
		if (known_size > 0)
			file.read(&out[0], known_size);
		return static_cast<bool>(file);
	}
	// Fallback (size unknown): original method
	std::ifstream file(filename, std::ios::binary);
	if (!file)
		return false;
	file.seekg(0, std::ios::end);
	const auto size = file.tellg();
	if (size < 0)
		return false;
	out.resize(static_cast<size_t>(size));
	file.seekg(0, std::ios::beg);
	if (size > 0)
		file.read(&out[0], size);
	return static_cast<bool>(file);
}

std::string read_entire_file_cached(const std::string& filename)
{
	const uint32_t current_call = call_counter.fetch_add(1) + 1;
	const auto now = std::chrono::steady_clock::now();

	{
		std::lock_guard<std::mutex> lock(cache_mutex);
		maybe_maintain_unlocked(current_call);
		auto it = file_cache.find(filename);
		if (it != file_cache.end())
		{
			if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second.last_check).count() < 1)
			{
				return it->second.content; // return immediately
			}
		}
	}

	struct stat st{};
	if (stat(filename.c_str(), &st) != 0)
		return ""; // not found or error
	const time_t new_mtime = st.st_mtime;
	const size_t file_size = static_cast<size_t>(st.st_size);

	std::string content;
	{
		std::lock_guard<std::mutex> lock(cache_mutex);
		auto it = file_cache.find(filename);
		if (it != file_cache.end())
		{
			it->second.last_check = now;
			if (it->second.mtime == new_mtime)
			{
				return it->second.content; // unchanged since last load
			}
			total_cache_size -= it->second.size;
			file_cache.erase(it);
		}
	}

	if (!read_file_binary(filename, content, file_size))
		return "";

	if (content.size() > global_config.file_cache_max_size)
	{
		return content;
	}

	{
		std::lock_guard<std::mutex> lock(cache_mutex);
		insert_entry_unlocked(filename, std::move(content), new_mtime);
		evict_size_unlocked();
		return file_cache[filename].content; // return stored copy
	}
}

bool write_entire_file(const std::string& filename, const std::string& content)
{
	std::ofstream file(filename, std::ios::binary | std::ios::trunc);
	if (!file)
		return false;
	file.write(content.data(), content.size());
	if (!file)
		return false;
	file.close();

	std::lock_guard<std::mutex> lock(cache_mutex);
	if (content.size() > global_config.file_cache_max_size)
	{
		remove_entry_unlocked(filename); // ensure not cached if too large
	}
	else
	{
		insert_entry_unlocked(filename, std::string(content), std::time(nullptr));
		evict_size_unlocked();
	}
	return true;
}

CacheStats get_cache_stats()
{
	std::lock_guard<std::mutex> lock(cache_mutex);
	return { file_cache.size(), total_cache_size, global_config.file_cache_max_size };
}