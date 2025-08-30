#pragma once

#include <string>

std::string read_entire_file_cached(const std::string& filename);
bool write_entire_file(const std::string& filename, const std::string& content);

struct CacheStats
{
	size_t total_entries;
	size_t total_size;
	size_t max_size;
};

CacheStats get_cache_stats();